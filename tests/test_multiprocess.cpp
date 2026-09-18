#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <thread>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

std::string worker_executable() { return RG_WORKER_EXECUTABLE; }
std::string ctl_executable() { return RG_CTL_EXECUTABLE; }

u16 pick_ephemeral_port() {
  std::string error;
  std::unique_ptr<Listener> listener = Listener::bind_loopback(0, error);
  if (listener == nullptr) {
    return 0;
  }
  const u16 port = listener->port();
  listener->close();
  return port;
}

// A real worker process listening on a real loopback port.
class WorkerProcess {
 public:
  bool start(const std::vector<std::string>& extra_arguments = {}) {
    port = pick_ephemeral_port();
    if (port == 0) {
      return false;
    }
    const std::string out_path = scratch.file("worker_out.txt");
    const std::string err_path = scratch.file("worker_err.txt");
    std::vector<std::string> arguments = {"--listen", "127.0.0.1:" + std::to_string(port),
                                          "--threads", "2", "--queue", "8"};
    for (const std::string& extra : extra_arguments) {
      arguments.push_back(extra);
    }
    std::string error;
    process = ChildProcess::start(worker_executable(), arguments, out_path, err_path, error);
    if (!process.valid()) {
      last_error = error;
      return false;
    }
    endpoint.host = "127.0.0.1";
    endpoint.port = port;
    return wait_until_reachable();
  }

  // Polls until the worker's listener accepts a connection. This is a
  // readiness poll for a real OS process, not a test timeout: if the worker
  // never comes up the test fails with the worker's own stderr attached.
  bool wait_until_reachable() {
    for (int attempt = 0; attempt < 400; ++attempt) {
      Socket probe;
      std::string error;
      if (Socket::connect_to(endpoint, probe, error)) {
        probe.close();
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    last_error = "worker did not become reachable; stderr: " + process.read_stderr();
    return false;
  }

  void kill() { process.kill(); }

  int wait() { return process.wait(); }

  [[nodiscard]] std::string stdout_text() const { return process.read_stdout(); }
  [[nodiscard]] std::string stderr_text() const { return process.read_stderr(); }

  ChildProcess process;
  ScratchDir scratch{"worker"};
  u16 port{0};
  Endpoint endpoint;
  std::string last_error;
};

struct Coordinator {
  explicit Coordinator(WorkerBootId boot, FabricEpoch epoch) : boot(boot), epoch(epoch) {
    EngineConfig config;
    config.boot = boot;
    config.initial_epoch = epoch;
    config.durability = DurabilityMode::None;
    engine = std::make_unique<RateGovernor>(config, clock, nullptr, nullptr, nullptr);
    ids = install_fixture(*engine, FixtureOptions{});
    actor = operator_context(epoch, boot, "multiprocess coordinator");
  }

  WorkerBootId boot{};
  FabricEpoch epoch{};
  ManualClock clock;
  std::unique_ptr<RateGovernor> engine;
  FixtureIds ids;
  ActorContext actor;
};

}  // namespace

RG_TEST(multiprocess, apply_and_readback_cross_a_real_process_boundary) {
  WorkerProcess worker;
  RG_REQUIRE(worker.start());
  Coordinator coordinator(WorkerBootId(4242), FabricEpoch(3));

  std::string error;
  std::unique_ptr<RemoteBackend> backend =
      RemoteBackend::connect(worker.endpoint, coordinator.epoch, coordinator.boot, error);
  if (backend == nullptr) {
    RG_NOTE("worker stderr: " + worker.stderr_text());
  }
  RG_REQUIRE(backend != nullptr);
  RG_CHECK(backend->hello().kind == BackendKind::Synthetic);
  RG_CHECK(backend->hello().verification == VerificationMode::PostApplyReadback);
  RG_CHECK(backend->session_live());
  RG_CHECK(backend->session_boot().valid());
  RG_CHECK(backend->session_boot() != coordinator.boot);
  RG_CHECK(!backend->session_description().empty());

  BackendDescriptor descriptor = backend->describe();
  (void)coordinator.engine->put_backend(descriptor);
  coordinator.engine->bind_backend(backend.get(), backend->session_epoch());

  const Result<RateEnvelopeId> opened = coordinator.engine->open_envelope(
      make_request(coordinator.ids, FixtureOptions{}), coordinator.actor);
  RG_REQUIRE(opened.has_value());
  const Result<ApplyDispatch> applied =
      coordinator.engine->apply(opened.value(), coordinator.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(applied.value().verified);
  RG_CHECK(applied.value().resulting_state == EnvelopeState::Applied);

  const Result<EnvelopeView> view = coordinator.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().legally_enforceable_now);
  RG_CHECK_EQ(view.value().applied_rate_ups, 1000ULL);
  RG_CHECK(view.value().applied_boot == backend->session_boot());

  // The claim really crossed a socket: bytes and frames are accounted for.
  RG_CHECK(backend->bytes_sent() > 0);
  RG_CHECK(backend->bytes_received() > 0);
  RG_CHECK(backend->round_trips() >= 2);

  AccountingResponseMessage accounting;
  RG_REQUIRE(backend->fetch_accounting(accounting, error));
  RG_CHECK(accounting.applies_applied >= 1);
  RG_CHECK_EQ(accounting.active_envelopes, 1ULL);
  RG_CHECK(accounting.boot == backend->session_boot());
  RG_CHECK_EQ(accounting.fenced_stale_epoch, 0ULL);

  const Result<RevokeDispatch> revoked = coordinator.engine->revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByOperator, coordinator.actor);
  RG_REQUIRE(revoked.has_value());
  RG_CHECK(revoked.value().verified);
  RG_REQUIRE(backend->fetch_accounting(accounting, error));
  RG_CHECK_EQ(accounting.active_envelopes, 0ULL);
  RG_CHECK(accounting.revokes_applied >= 1);

  const u64 worker_boot = backend->session_boot().value();
  backend->shutdown();
  const int exit_code = worker.wait();
  RG_CHECK_EQ(exit_code, 0);
  const std::string worker_out = worker.stdout_text();
  RG_CHECK(worker_out.find("listening") != std::string::npos);
  RG_CHECK(worker_out.find("\"applies\":") != std::string::npos);
  const std::string boot_token = "\"worker_boot\":" + std::to_string(worker_boot);
  RG_CHECK(worker_out.find(boot_token) != std::string::npos);
}

RG_TEST(multiprocess, worker_death_leaves_the_effect_unknown) {
  WorkerProcess worker;
  RG_REQUIRE(worker.start());
  Coordinator coordinator(WorkerBootId(5150), FabricEpoch(4));
  std::string error;
  std::unique_ptr<RemoteBackend> backend =
      RemoteBackend::connect(worker.endpoint, coordinator.epoch, coordinator.boot, error);
  RG_REQUIRE(backend != nullptr);
  BackendDescriptor descriptor = backend->describe();
  (void)coordinator.engine->put_backend(descriptor);
  coordinator.engine->bind_backend(backend.get(), backend->session_epoch());
  const Result<RateEnvelopeId> opened = coordinator.engine->open_envelope(
      make_request(coordinator.ids, FixtureOptions{}), coordinator.actor);
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(coordinator.engine->apply(opened.value(), coordinator.actor).has_value());

  // Kill the enforcement process outright.
  worker.kill();
  RG_CHECK(worker.wait() != 0);

  const Result<RevokeDispatch> revoked = coordinator.engine->revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByOperator, coordinator.actor);
  RG_REQUIRE(revoked.has_value());
  RG_CHECK(!revoked.value().verified);
  RG_CHECK(!backend->session_live());

  const Result<EnvelopeView> view = coordinator.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);

  // A fresh apply cannot even be attempted: the engine learns the session is
  // gone and refuses, demoting every claim that session supported.
  const Result<ApplyDispatch> retry = coordinator.engine->apply(opened.value(), coordinator.actor);
  RG_CHECK(!retry.has_value());
  RG_CHECK(retry.code() == ErrorCode::BackendFailure);
  RG_CHECK(retry.reason() == ReasonCode::BackendSessionLost);
  const Result<EnvelopeView> after_loss = coordinator.engine->inspect(opened.value());
  RG_REQUIRE(after_loss.has_value());
  RG_CHECK(after_loss.value().requires_revalidation);
  RG_CHECK(!after_loss.value().effect_verified);
}

RG_TEST(multiprocess, worker_restart_requires_revalidation_and_a_fresh_apply) {
  WorkerProcess first_worker;
  RG_REQUIRE(first_worker.start());
  const u16 port = first_worker.port;
  Coordinator coordinator(WorkerBootId(6001), FabricEpoch(6));
  std::string error;
  std::unique_ptr<RemoteBackend> backend =
      RemoteBackend::connect(first_worker.endpoint, coordinator.epoch, coordinator.boot, error);
  RG_REQUIRE(backend != nullptr);
  const WorkerBootId first_boot = backend->session_boot();
  BackendDescriptor descriptor = backend->describe();
  (void)coordinator.engine->put_backend(descriptor);
  coordinator.engine->bind_backend(backend.get(), backend->session_epoch());
  const Result<RateEnvelopeId> opened = coordinator.engine->open_envelope(
      make_request(coordinator.ids, FixtureOptions{}), coordinator.actor);
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(coordinator.engine->apply(opened.value(), coordinator.actor).has_value());

  // The enforcement process dies and is replaced by a fresh incarnation on the
  // same endpoint. A recovery-style revalidation must not assume the effect
  // survived.
  first_worker.kill();
  RG_CHECK(first_worker.wait() != 0);
  backend->mark_dead("worker was killed for the restart scenario");
  // The engine must notice the loss rather than keep presenting the old effect
  // as current.
  const TickReport loss_tick = coordinator.engine->tick();
  RG_CHECK(loss_tick.now_ns >= 0);
  const Result<EnvelopeView> lost_view = coordinator.engine->inspect(opened.value());
  RG_REQUIRE(lost_view.has_value());
  RG_CHECK(lost_view.value().requires_revalidation);
  RG_CHECK(!lost_view.value().effect_verified);

  WorkerProcess second_worker;
  second_worker.port = port;
  second_worker.endpoint.host = "127.0.0.1";
  second_worker.endpoint.port = port;
  {
    std::vector<std::string> arguments = {"--listen", "127.0.0.1:" + std::to_string(port),
                                          "--epoch", std::to_string(coordinator.epoch.value())};
    std::string start_error;
    second_worker.process =
        ChildProcess::start(worker_executable(), arguments, second_worker.scratch.file("out.txt"),
                            second_worker.scratch.file("err.txt"), start_error);
    RG_REQUIRE(second_worker.process.valid());
    RG_REQUIRE(second_worker.wait_until_reachable());
  }

  std::unique_ptr<RemoteBackend> replacement =
      RemoteBackend::connect(second_worker.endpoint, coordinator.epoch, coordinator.boot, error);
  RG_REQUIRE(replacement != nullptr);
  RG_CHECK(replacement->session_boot() != first_boot);
  coordinator.engine->bind_backend(replacement.get(), replacement->session_epoch());

  const RevalidationReport report = coordinator.engine->revalidate_all(coordinator.actor);
  RG_CHECK_EQ(report.requeued, 1ULL);
  RG_CHECK_EQ(report.confirmed_applied, 0ULL);
  RG_CHECK_EQ(report.unknown, 0ULL);
  const Result<EnvelopeView> view = coordinator.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);

  const Result<ApplyDispatch> reapplied =
      coordinator.engine->apply(opened.value(), coordinator.actor);
  RG_REQUIRE(reapplied.has_value());
  RG_CHECK(reapplied.value().verified);
  replacement->shutdown();
  RG_CHECK_EQ(second_worker.wait(), 0);
}

RG_TEST(multiprocess, stale_epoch_and_boot_are_fenced_by_the_worker) {
  WorkerProcess worker;
  RG_REQUIRE(worker.start({"--epoch", "5", "--verbose"}));

  // An epoch older than the one the worker has already observed is refused.
  {
    std::string error;
    std::unique_ptr<RemoteBackend> stale =
        RemoteBackend::connect(worker.endpoint, FabricEpoch(4), WorkerBootId(1), error);
    RG_CHECK(stale == nullptr);
    RG_CHECK(error.find("StaleEpoch") != std::string::npos);
  }

  // A first incarnation at epoch 5 is admitted.
  std::string error;
  std::unique_ptr<RemoteBackend> first =
      RemoteBackend::connect(worker.endpoint, FabricEpoch(5), WorkerBootId(100), error);
  RG_REQUIRE(first != nullptr);

  // A different incarnation claiming the same epoch is refused: the worker
  // cannot tell which of the two is entitled to mutate, so it refuses both.
  {
    std::string second_error;
    std::unique_ptr<RemoteBackend> conflicting =
        RemoteBackend::connect(worker.endpoint, FabricEpoch(5), WorkerBootId(200), second_error);
    RG_CHECK(conflicting == nullptr);
    RG_CHECK(second_error.find("StaleBoot") != std::string::npos);
  }

  // A strictly newer epoch is admitted and fences everything held before it.
  std::unique_ptr<RemoteBackend> advanced =
      RemoteBackend::connect(worker.endpoint, FabricEpoch(6), WorkerBootId(300), error);
  RG_REQUIRE(advanced != nullptr);
  RG_CHECK_EQ(advanced->session_epoch().value(), 6ULL);

  AccountingResponseMessage accounting;
  RG_REQUIRE(advanced->fetch_accounting(accounting, error));
  // The refused handshakes happened in their own sessions, so the aggregate is
  // read from the worker's own exit report rather than from one session.
  advanced->shutdown();
  RG_CHECK_EQ(worker.wait(), 0);
  const std::string worker_out = worker.stdout_text();
  RG_CHECK(worker_out.find("\"fenced_stale_epoch\":0") == std::string::npos);
  RG_CHECK(worker_out.find("\"fenced_stale_boot\":0") == std::string::npos);
  RG_CHECK(worker_out.find("\"fenced_stale_epoch\":") != std::string::npos);
  RG_CHECK(worker_out.find("\"fenced_stale_boot\":") != std::string::npos);
}

RG_TEST(multiprocess, malformed_frames_are_refused_by_a_real_worker) {
  WorkerProcess worker;
  RG_REQUIRE(worker.start());

  // Raw garbage with no handshake at all.
  {
    Socket socket;
    std::string error;
    RG_REQUIRE(Socket::connect_to(worker.endpoint, socket, error));
    RG_CHECK(socket.send_raw(std::string(64, 'Z'), error));
    Frame frame;
    ReasonCode reason = ReasonCode::None;
    // The worker either answers with an error frame or drops the connection.
    if (socket.recv_frame(frame, error, reason)) {
      RG_CHECK(frame.type == MessageType::ErrorResponse);
    }
    socket.close();
  }

  // A frame whose declared length exceeds the protocol bound.
  {
    Socket socket;
    std::string error;
    RG_REQUIRE(Socket::connect_to(worker.endpoint, socket, error));
    std::string header;
    header.resize(kWireHeaderSize);
    const u32 magic = kWireFrameMagic;
    const u16 version = kWireProtocolVersion;
    const u16 type = 0;
    const u32 flags = 0;
    const u32 length = kWireMaxPayloadBytes + 1;
    std::memcpy(header.data(), &magic, 4);
    std::memcpy(header.data() + 4, &version, 2);
    std::memcpy(header.data() + 6, &type, 2);
    std::memcpy(header.data() + 8, &flags, 4);
    std::memcpy(header.data() + 12, &length, 4);
    RG_CHECK(socket.send_raw(header, error));
    Frame frame;
    ReasonCode reason = ReasonCode::None;
    if (socket.recv_frame(frame, error, reason)) {
      RG_CHECK(frame.type == MessageType::ErrorResponse);
    }
    socket.close();
  }

  // A truncated frame followed by a close.
  {
    Socket socket;
    std::string error;
    RG_REQUIRE(Socket::connect_to(worker.endpoint, socket, error));
    const std::vector<std::byte> encoded =
        encode_frame(Frame{MessageType::Hello, 0, std::vector<std::byte>(8, std::byte{1})});
    RG_REQUIRE(!encoded.empty());
    RG_CHECK(socket.send_raw(std::string(reinterpret_cast<const char*>(encoded.data()), 10), error));
    socket.close();
  }

  // The worker survived all of it and still serves a clean session.
  Coordinator coordinator(WorkerBootId(7001), FabricEpoch(9));
  std::string error;
  std::unique_ptr<RemoteBackend> backend =
      RemoteBackend::connect(worker.endpoint, coordinator.epoch, coordinator.boot, error);
  if (backend == nullptr) {
    RG_NOTE("worker stderr: " + worker.stderr_text());
  }
  RG_REQUIRE(backend != nullptr);
  RG_CHECK(backend->session_live());
  AccountingResponseMessage accounting;
  RG_REQUIRE(backend->fetch_accounting(accounting, error));
  RG_CHECK_EQ(accounting.active_envelopes, 0ULL);
  backend->shutdown();
  RG_CHECK_EQ(worker.wait(), 0);
  // The per-session counters are aggregated by the listener, so the frames that
  // the *first* sessions rejected are still visible in the worker's own report.
  const std::string worker_out = worker.stdout_text();
  RG_CHECK(worker_out.find("\"malformed_frames\":") != std::string::npos);
  RG_CHECK(worker_out.find("\"malformed_frames\":0") == std::string::npos);
}

RG_TEST(multiprocess, coordinator_process_restart_through_rgctl) {
  WorkerProcess worker;
  RG_REQUIRE(worker.start());
  ScratchDir scratch("ctl");
  const std::string journal = scratch.file("coordinator.rgjournal");
  const std::string endpoint = "127.0.0.1:" + std::to_string(worker.port);

  // Process 1: a coordinator incarnation that applies an envelope.
  const ProcessRun applied =
      run_process(ctl_executable(),
                  {"apply", "--journal", journal, "--endpoint", endpoint, "--ceiling", "4000"},
                  "ctl_apply");
  RG_REQUIRE(applied.started);
  if (applied.exit_code != 0) {
    RG_NOTE("apply stdout: " + applied.out + " stderr: " + applied.err);
  }
  RG_REQUIRE(applied.exit_code == 0);
  RG_CHECK(applied.out.find("verified=yes") != std::string::npos);

  // Process 2: a read-only inspection of the durable record.
  const ProcessRun inspected =
      run_process(ctl_executable(), {"inspect", "--journal", journal}, "ctl_inspect");
  RG_REQUIRE(inspected.exit_code == 0);
  RG_CHECK(inspected.out.find("DURABLE RECORD ONLY") != std::string::npos);
  RG_CHECK(inspected.out.find("env:1") != std::string::npos);

  // Process 3: a recovery in a brand-new incarnation.
  const ProcessRun recovered =
      run_process(ctl_executable(), {"recover", "--journal", journal, "--endpoint", endpoint},
                  "ctl_recover");
  RG_REQUIRE(recovered.exit_code == 0);
  RG_CHECK(recovered.out.find("liveness_restored=no") != std::string::npos);
  RG_CHECK(recovered.out.find("enforceable_now=no") != std::string::npos);
  RG_CHECK(recovered.out.find("requires_revalidation") != std::string::npos ||
           recovered.out.find("REVALIDATE") != std::string::npos);

  // Process 4: revalidation against the live worker. The epoch advanced, so the
  // worker cleared its table and the effect must be re-established.
  const ProcessRun revalidated = run_process(
      ctl_executable(), {"revalidate", "--journal", journal, "--endpoint", endpoint},
      "ctl_revalidate");
  RG_REQUIRE(revalidated.exit_code == 0);
  RG_CHECK(revalidated.out.find("requeued=1") != std::string::npos);

  // Process 5: a fresh apply in yet another incarnation.
  const ProcessRun reapplied =
      run_process(ctl_executable(),
                  {"apply", "--journal", journal, "--endpoint", endpoint, "--ceiling", "4000"},
                  "ctl_apply_again");
  if (reapplied.exit_code != 0) {
    RG_NOTE("reapply stdout: " + reapplied.out + " stderr: " + reapplied.err);
  }
  RG_REQUIRE(reapplied.exit_code == 0);
  RG_CHECK(reapplied.out.find("verified=yes") != std::string::npos);
  RG_CHECK(reapplied.out.find("enforceable_now=yes") != std::string::npos);

  // A journal scan in yet another process must find no uncommitted records.
  const ProcessRun scanned = run_process(ctl_executable(), {"journal-scan", "--journal", journal},
                                         "ctl_scan");
  RG_REQUIRE(scanned.exit_code == 0);
  RG_CHECK(scanned.out.find("discarded_uncommitted=0") != std::string::npos);

  worker.kill();
  (void)worker.wait();
}
