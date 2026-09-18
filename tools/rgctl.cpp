// rgctl - operator tool for Rate Governor.
//
// Subcommands either exercise the engine deterministically offline, or operate
// on a durable journal so that a coordinator incarnation can be started and
// stopped as a real OS process. Every rate figure printed here is a synthetic
// control-plane value: no packets are involved.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "cli_common.hpp"
#include "rate_governor/rate_governor.hpp"
#include "scenario.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace rate_governor;
using rate_governor::tools::Arguments;
using rate_governor::tools::yes_no;

rate_governor::u64 current_process_id() {
#if defined(_WIN32)
  return static_cast<rate_governor::u64>(::_getpid());
#else
  return static_cast<rate_governor::u64>(::getpid());
#endif
}

struct JournaledEngine {
  // The engine holds raw pointers to the clock and the journal, so both must
  // outlive it: declaration order is load bearing.
  SteadyClock clock;
  std::unique_ptr<Journal> journal;
  std::unique_ptr<RateGovernor> engine;
  RecoveryReport recovery;
  bool recovered{false};
};

u64 fresh_boot_id() {
  const u64 raw = mix64(current_process_id()) ^ mix64(SteadyClock().NowNs());
  return raw == 0 ? 1 : raw;
}

// Opens an existing journal through a full recovery, or creates a new one.
// The two paths are deliberately distinct: recovery always advances the fabric
// epoch and demotes live claims, while creation is a genuine first boot.
bool open_or_recover(const std::string& path, bool allow_create, JournaledEngine& out,
                     std::string& error) {
  FILE* probe = std::fopen(path.c_str(), "rb");
  const bool exists = probe != nullptr;
  if (probe != nullptr) {
    std::fclose(probe);
  }
  EngineConfig config;
  config.boot = WorkerBootId(fresh_boot_id());
  config.durability = DurabilityMode::Strict;

  if (exists) {
    Result<RecoveredEngine> recovered = recover_engine(path, config, out.clock, nullptr, true);
    if (!recovered) {
      error = recovered.message();
      return false;
    }
    out.recovery = recovered.value().report;
    out.journal = std::move(recovered.value().journal);
    out.engine = std::move(recovered.value().engine);
    out.recovered = true;
    return true;
  }
  if (!allow_create) {
    error = "journal does not exist";
    return false;
  }
  JournalConfig journal_config;
  journal_config.path = path;
  JournalResult open_result;
  out.journal = Journal::open(journal_config, true, open_result);
  if (!out.journal) {
    error = open_result.message;
    return false;
  }
  out.engine = std::make_unique<RateGovernor>(config, out.clock, nullptr, out.journal.get(),
                                              nullptr);
  return true;
}

void print_recovery(const RecoveryReport& report) {
  std::printf("recovery: records=%llu transactions=%llu discarded_uncommitted=%llu "
              "truncated_bytes=%llu tail_truncated=%s\n",
              static_cast<unsigned long long>(report.committed_records),
              static_cast<unsigned long long>(report.committed_transactions),
              static_cast<unsigned long long>(report.discarded_uncommitted_records),
              static_cast<unsigned long long>(report.discarded_truncated_bytes),
              rate_governor::tools::yes_no(report.truncated_tail));
  std::printf("recovery: epoch %llu -> %llu boot %llu -> %llu liveness_restored=%s "
              "backend_bound=%s\n",
              static_cast<unsigned long long>(report.recovered_epoch.value()),
              static_cast<unsigned long long>(report.new_epoch.value()),
              static_cast<unsigned long long>(report.previous_boot.value()),
              static_cast<unsigned long long>(report.new_boot.value()),
              rate_governor::tools::yes_no(report.liveness_restored),
              rate_governor::tools::yes_no(report.backend_bound));
  std::printf("recovery: envelopes=%llu attempts=%llu ambiguous_attempts=%llu "
              "effects_demoted=%llu requires_revalidation=%llu revoke_pending=%llu\n",
              static_cast<unsigned long long>(report.envelopes),
              static_cast<unsigned long long>(report.attempts),
              static_cast<unsigned long long>(report.attempts_marked_ambiguous),
              static_cast<unsigned long long>(report.effect_claims_demoted),
              static_cast<unsigned long long>(report.envelopes_requiring_revalidation),
              static_cast<unsigned long long>(report.envelopes_revoke_pending));
  std::printf("recovery: %s\n", report.explanation.c_str());
}

void print_views(RateGovernor& engine) {
  const std::vector<EnvelopeView> views = engine.list_envelopes(64);
  for (const EnvelopeView& view : views) {
    std::printf("view: %s\n", describe(view).c_str());
  }
  if (views.empty()) {
    std::printf("view: (no envelopes)\n");
  }
}

int command_version() {
  std::printf("%s %s\n", std::string(kProductName).c_str(), std::string(kVersionString).c_str());
  std::printf("%s\n", std::string(kCopyrightNotice).c_str());
  std::printf("journal format %u, wire protocol %u, C++20\n",
              static_cast<unsigned>(kJournalFormatVersion),
              static_cast<unsigned>(kWireProtocolVersion));
  std::printf("enforcement backends shipped: synthetic (software table, no physical shaping)\n");
  return 0;
}

int command_selfcheck() {
  int failures = 0;
  auto check = [&failures](const char* name, bool ok) {
    std::printf("selfcheck: %-34s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
      ++failures;
    }
  };

  check("mul_div exact (10^9 * 3 / 10^9 == 3)", [] {
    u64 out = 0;
    return mul_div(1000000000ULL, 3, 1000000000ULL, out) && out == 3;
  }());
  check("mul_div overflow detected", [] {
    u64 out = 0;
    return !mul_div(kU64Max, 2, 1, out);
  }());
  check("checked_add overflow detected", [] {
    u64 out = 0;
    return !checked_add(kU64Max, 1, out);
  }());

  ManualClock clock(0);
  TokenBucket bucket;
  // 1000 tokens per second, a one-second capacity: half a second must accrue
  // exactly half the bucket, and the split-interval carry must not lose tokens.
  configure_bucket(bucket, 1000, 1000, 0);
  (void)try_consume(bucket, 1000);
  (void)refill_bucket(bucket, 500000000);
  const u64 half = bucket.tokens;
  (void)refill_bucket(bucket, 1000000000);
  check("token bucket accrues exactly", half == 500 && bucket.tokens == 1000);
  TokenBucket split;
  configure_bucket(split, 10, 1, 0);
  (void)try_consume(split, 10);
  for (int index = 0; index < 1000; ++index) {
    (void)refill_bucket(split, static_cast<TimestampNs>(index + 1) * 1000000);
  }
  check("token bucket split intervals agree", split.tokens == 1);

  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, config.boot,
                           config.initial_epoch, "selfcheck");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);
  rate_governor::tools::ScenarioOptions options;
  options.epoch = engine.epoch();
  options.boot = engine.boot();
  const rate_governor::tools::ScenarioIds ids = rate_governor::tools::install_scenario(engine, options);
  const ActorContext actor = rate_governor::tools::scenario_actor(options);
  BackendDescriptor descriptor = backend.describe();
  descriptor.provenance = rate_governor::tools::scenario_provenance("selfcheck backend");
  (void)engine.put_backend(descriptor);
  check("authority installation", engine.storage_stats().grants == 1);

  EnvelopeRequest request;
  request.flow = ids.flow;
  request.resource = ids.resource;
  request.grant = ids.grant;
  request.policy = ids.policy;
  request.backend = ids.backend;
  request.has_reservation = true;
  request.reservation = ids.reservation;
  Result<RateEnvelopeId> opened = engine.open_envelope(request, actor);
  check("envelope opened", opened.has_value());
  if (!opened) {
    return 1;
  }
  Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
  check("apply verified", applied.has_value() && applied.value().verified);
  Result<EnvelopeView> view = engine.inspect(opened.value());
  check("envelope enforceable", view.has_value() && view.value().legally_enforceable_now);
  if (view.has_value()) {
    std::printf("selfcheck: %s\n", describe(view.value()).c_str());
  }

  const Frame frame_probe = [] {
    Frame frame;
    frame.type = MessageType::HelloAck;
    frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    return frame;
  }();
  const std::vector<std::byte> encoded = encode_frame(frame_probe);
  Frame decoded;
  ReasonCode reason = ReasonCode::None;
  check("wire frame round trip",
        decode_frame(encoded, decoded, reason) && decoded.payload == frame_probe.payload);

  std::printf("selfcheck: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}

int command_demo(const Arguments& args) {
  const u64 ceiling = args.get_u64("--ceiling", 1000);
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  SyntheticDeviceConfig device;
  SyntheticBackend backend(BackendId(1), Generation(1), device, config.boot, config.initial_epoch,
                           "demo synthetic device");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);

  rate_governor::tools::ScenarioOptions options;
  options.epoch = engine.epoch();
  options.boot = engine.boot();
  options.ceiling_ups = ceiling;
  options.target_ups = ceiling * 6 / 10;
  options.floor_ups = ceiling / 10;
  const rate_governor::tools::ScenarioIds ids = rate_governor::tools::install_scenario(engine, options);
  const ActorContext actor = rate_governor::tools::scenario_actor(options);
  BackendDescriptor descriptor = backend.describe();
  descriptor.provenance = rate_governor::tools::scenario_provenance("demo backend descriptor");
  (void)engine.put_backend(descriptor);

  EnvelopeRequest request;
  request.flow = ids.flow;
  request.resource = ids.resource;
  request.grant = ids.grant;
  request.policy = ids.policy;
  request.backend = ids.backend;
  request.has_reservation = true;
  request.reservation = ids.reservation;
  Result<RateEnvelopeId> opened = engine.open_envelope(request, actor);
  if (!opened) {
    std::printf("demo: cannot open envelope: %s\n", opened.message().c_str());
    return 1;
  }
  const RateEnvelopeId envelope = opened.value();

  Result<PlanOutcome> plan = engine.explain_plan(envelope);
  if (plan) {
    std::printf("demo: plan authorized=%s reason=%s\n", yes_no(plan.value().authorized),
                std::string(to_string_view(plan.value().reason)).c_str());
    std::printf("demo: %s\n", plan.value().explanation.c_str());
  }
  Result<ApplyDispatch> applied = engine.apply(envelope, actor);
  std::printf("demo: apply=%s\n",
              applied ? applied.value().explanation.c_str() : applied.message().c_str());
  Result<EnvelopeView> view = engine.inspect(envelope);
  if (view) {
    std::printf("demo: %s\n", describe(view.value()).c_str());
    std::printf("demo: burst remaining=%llu of %llu\n",
                static_cast<unsigned long long>(view.value().burst_tokens_available),
                static_cast<unsigned long long>(view.value().burst_capacity));
  }

  clock.advance(1000);
  Result<u64> spent = engine.consume_burst(envelope, 100);
  std::printf("demo: consume_burst(100)=%s\n",
              spent ? std::to_string(spent.value()).c_str() : spent.message().c_str());

  const TickReport tick = engine.tick();
  std::printf("demo: tick refilled=%llu stale=%llu revokes_pending=%llu\n",
              static_cast<unsigned long long>(tick.envelopes_refilled),
              static_cast<unsigned long long>(tick.envelopes_stale),
              static_cast<unsigned long long>(tick.revokes_pending));

  // Recall the funding grant in the same instant as a fresh apply attempt: the
  // attempt must not be able to publish success afterwards.
  Result<ApplyDispatch> second = engine.apply(envelope, actor);
  std::printf("demo: second apply duplicate=%s\n",
              second ? yes_no(second.value().duplicate) : second.message().c_str());

  const Status recalled = engine.change_grant_state(ids.grant, Generation(1), GrantState::Recalled,
                                                    ReasonCode::EnvelopeRevokedByGrantRecall, actor);
  std::printf("demo: grant recall=%s\n", recalled.has_value() ? "ok" : recalled.message().c_str());
  const TickReport after = engine.tick();
  std::printf("demo: after recall stale=%llu revoke_pending=%llu\n",
              static_cast<unsigned long long>(after.envelopes_stale),
              static_cast<unsigned long long>(after.revokes_pending));
  Result<EnvelopeView> recalled_view = engine.inspect(envelope);
  if (recalled_view) {
    std::printf("demo: %s\n", describe(recalled_view.value()).c_str());
  }
  return 0;
}

int command_inspect(const Arguments& args) {
  const std::string path = args.get("--journal");
  if (path.empty()) {
    std::fputs("rgctl inspect: --journal is required\n", stderr);
    return 2;
  }
  const JournalScanResult scan = scan_journal_file(path);
  if (!scan.ok) {
    std::printf("inspect: journal rejected: %s\n", scan.message.c_str());
    return 1;
  }
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  RateGovernor engine(config, clock, nullptr, nullptr, nullptr);
  engine.apply_snapshot_records(scan.committed);
  // A read-only inspection must not present durable history as current effect:
  // the in-memory demotion changes nothing on disk and marks every claim as
  // requiring revalidation, exactly as a real recovery would.
  {
    const ActorContext actor =
        operator_context(engine.epoch(), engine.boot(), "rgctl read-only inspection");
    u64 attempts_marked = 0;
    u64 envelopes_demoted = 0;
    u64 effects_demoted = 0;
    (void)engine.demote_for_recovery(actor, attempts_marked, envelopes_demoted, effects_demoted);
  }
  std::printf("inspect: committed_records=%llu transactions=%llu truncated_tail=%s\n",
              static_cast<unsigned long long>(scan.committed.size()),
              static_cast<unsigned long long>(scan.committed_transactions),
              rate_governor::tools::yes_no(scan.truncated_tail));
  std::printf(
      "inspect: DURABLE RECORD ONLY - effect claims below are history, not current enforcement; "
      "revalidate before trusting them\n");
  print_views(engine);
  const EngineStorageStats stats = engine.storage_stats();
  std::printf("inspect: grants=%llu policies=%llu envelopes=%llu attempts=%llu orphan_records=%llu\n",
              static_cast<unsigned long long>(stats.grants),
              static_cast<unsigned long long>(stats.policies),
              static_cast<unsigned long long>(stats.envelopes),
              static_cast<unsigned long long>(stats.attempts),
              static_cast<unsigned long long>(stats.recovery_orphan_records));
  return 0;
}

struct CoordinatorSession {
  JournaledEngine owned;
  std::unique_ptr<RemoteBackend> backend;
  BackendId backend_id{BackendId(1)};
  Generation backend_generation{Generation(1)};
  ActorContext actor;
};

bool start_coordinator(const Arguments& args, CoordinatorSession& session, std::string& error) {
  const std::string path = args.get("--journal");
  if (path.empty()) {
    error = "--journal is required";
    return false;
  }
  if (!open_or_recover(path, true, session.owned, error)) {
    return false;
  }
  const std::string endpoint_text = args.get("--endpoint");
  Endpoint endpoint;
  if (!endpoint_text.empty() && !Endpoint::parse(endpoint_text, endpoint)) {
    error = "--endpoint must be host:port";
    return false;
  }
  if (!endpoint_text.empty()) {
    session.backend = RemoteBackend::connect(endpoint, session.owned.engine->epoch(),
                                             session.owned.engine->boot(), error);
    if (!session.backend) {
      return false;
    }
    session.backend_id = session.backend->describe().id;
    session.backend_generation = session.backend->describe().generation;
    session.owned.engine->bind_backend(session.backend.get(), session.backend->session_epoch());
  }
  session.actor = operator_context(session.owned.engine->epoch(), session.owned.engine->boot(),
                                   "rgctl coordinator");
  return true;
}

rate_governor::tools::ScenarioIds ensure_authority(CoordinatorSession& session,
                                                   const Arguments& args) {
  rate_governor::tools::ScenarioOptions options;
  options.ceiling_ups = args.get_u64("--ceiling", 1000);
  options.target_ups = options.ceiling_ups * 6 / 10;
  options.floor_ups = options.ceiling_ups / 10;
  options.burst_tokens = args.get_u64("--burst", 2000);
  options.epoch = session.owned.engine->epoch();
  options.boot = session.owned.engine->boot();
  const rate_governor::tools::ScenarioIds ids =
      rate_governor::tools::install_scenario(*session.owned.engine, options);
  if (session.backend) {
    BackendDescriptor descriptor = session.backend->describe();
    (void)session.owned.engine->put_backend(descriptor);
  }
  return ids;
}

int command_apply(const Arguments& args) {
  CoordinatorSession session;
  std::string error;
  if (!start_coordinator(args, session, error)) {
    std::printf("apply: cannot start coordinator: %s\n", error.c_str());
    return 1;
  }
  if (session.owned.recovered) {
    print_recovery(session.owned.recovery);
  }
  const rate_governor::tools::ScenarioIds ids = ensure_authority(session, args);
  std::printf("apply: epoch=%s boot=%s backend=%s\n",
              session.owned.engine->epoch().to_string().c_str(),
              session.owned.engine->boot().to_string().c_str(),
              session.backend ? session.backend->session_description().c_str() : "in-process none");

  EnvelopeRequest request;
  request.flow = ids.flow;
  request.resource = ids.resource;
  request.grant = ids.grant;
  request.policy = ids.policy;
  request.backend = ids.backend;
  request.has_reservation = true;
  request.reservation = ids.reservation;
  Result<RateEnvelopeId> opened = session.owned.engine->open_envelope(request, session.actor);
  if (!opened) {
    std::printf("apply: cannot open envelope: %s\n", opened.message().c_str());
    return 1;
  }
  RateEnvelopeId target = opened.value();
  const u64 requested_envelope = args.get_u64("--envelope", 0);
  if (requested_envelope != 0) {
    target = RateEnvelopeId(requested_envelope);
  }
  Result<ApplyDispatch> applied = session.owned.engine->apply(target, session.actor);
  if (!applied) {
    std::printf("apply: refused: %s\n", applied.message().c_str());
    print_views(*session.owned.engine);
    return 1;
  }
  std::printf("apply: attempt=%s verified=%s state=%s\n",
              applied.value().attempt.to_string().c_str(),
              rate_governor::tools::yes_no(applied.value().verified),
              std::string(to_string_view(applied.value().resulting_state)).c_str());
  print_views(*session.owned.engine);
  if (session.backend) {
    AccountingResponseMessage accounting;
    if (session.backend->fetch_accounting(accounting, error)) {
      std::printf("apply: worker accounting applies=%llu active_envelopes=%llu round_trips=%llu\n",
                  static_cast<unsigned long long>(accounting.applies_applied),
                  static_cast<unsigned long long>(accounting.active_envelopes),
                  static_cast<unsigned long long>(session.backend->round_trips()));
    }
  }
  return applied.value().verified ? 0 : 1;
}

int command_recover(const Arguments& args) {
  CoordinatorSession session;
  std::string error;
  if (!start_coordinator(args, session, error)) {
    std::printf("recover: %s\n", error.c_str());
    return 1;
  }
  if (!session.owned.recovered) {
    std::printf("recover: journal did not exist; a fresh incarnation was created\n");
  } else {
    print_recovery(session.owned.recovery);
  }
  print_views(*session.owned.engine);
  return 0;
}

int command_revalidate(const Arguments& args) {
  CoordinatorSession session;
  std::string error;
  if (!start_coordinator(args, session, error)) {
    std::printf("revalidate: %s\n", error.c_str());
    return 1;
  }
  if (!session.backend) {
    std::printf("revalidate: --endpoint is required to revalidate an effect\n");
    return 1;
  }
  const RevalidationReport report = session.owned.engine->revalidate_all(session.actor);
  std::printf("revalidate: considered=%llu confirmed=%llu requeued=%llu unknown=%llu denied=%llu "
              "skipped=%llu\n",
              static_cast<unsigned long long>(report.considered),
              static_cast<unsigned long long>(report.confirmed_applied),
              static_cast<unsigned long long>(report.requeued),
              static_cast<unsigned long long>(report.unknown),
              static_cast<unsigned long long>(report.denied),
              static_cast<unsigned long long>(report.skipped_no_backend));
  for (const RevalidationItem& item : report.items) {
    std::printf("revalidate: %s %s -> %s (%s) %s\n", item.envelope.to_string().c_str(),
                std::string(to_string_view(item.before)).c_str(),
                std::string(to_string_view(item.after)).c_str(),
                std::string(to_string_view(item.reason)).c_str(), item.explanation.c_str());
  }
  print_views(*session.owned.engine);
  return 0;
}

int command_revoke(const Arguments& args) {
  CoordinatorSession session;
  std::string error;
  if (!start_coordinator(args, session, error)) {
    std::printf("revoke: %s\n", error.c_str());
    return 1;
  }
  if (!session.backend) {
    std::printf("revoke: --endpoint is required to revoke an enforced envelope\n");
    return 1;
  }
  const u64 raw = args.get_u64("--envelope", 1);
  Result<RevokeDispatch> revoked =
      session.owned.engine->revoke(RateEnvelopeId(raw), ReasonCode::EnvelopeRevokedByOperator,
                                   session.actor);
  if (!revoked) {
    std::printf("revoke: refused: %s\n", revoked.message().c_str());
    return 1;
  }
  std::printf("revoke: attempt=%s verified=%s state=%s duplicate=%s\n",
              revoked.value().attempt.to_string().c_str(),
              rate_governor::tools::yes_no(revoked.value().verified),
              std::string(to_string_view(revoked.value().resulting_state)).c_str(),
              rate_governor::tools::yes_no(revoked.value().duplicate));
  print_views(*session.owned.engine);
  return revoked.value().verified ? 0 : 1;
}

int command_journal_scan(const Arguments& args) {
  const std::string path = args.get("--journal");
  if (path.empty()) {
    std::fputs("rgctl journal-scan: --journal is required\n", stderr);
    return 2;
  }
  const JournalScanResult scan = scan_journal_file(path);
  std::printf("journal-scan: ok=%s status=%s records=%llu transactions=%llu "
              "discarded_uncommitted=%llu truncated_bytes=%llu tail=%s\n",
              rate_governor::tools::yes_no(scan.ok),
              std::string(to_string_view(scan.status)).c_str(),
              static_cast<unsigned long long>(scan.committed.size()),
              static_cast<unsigned long long>(scan.committed_transactions),
              static_cast<unsigned long long>(scan.discarded_uncommitted_records),
              static_cast<unsigned long long>(scan.discarded_truncated_bytes),
              rate_governor::tools::yes_no(scan.truncated_tail));
  std::printf("journal-scan: %s\n", scan.message.c_str());
  return scan.ok ? 0 : 1;
}

void print_usage() {
  std::fputs(
      "rgctl - Rate Governor operator tool\n"
      "\n"
      "usage: rgctl <command> [options]\n"
      "\n"
      "  version                       print version and build facts\n"
      "  selfcheck                     run offline engine self checks\n"
      "  demo [--ceiling N]            deterministic offline lifecycle walkthrough\n"
      "  inspect --journal J           read the durable record without changing it\n"
      "  apply --journal J [--endpoint H:P] [--ceiling N]\n"
      "  recover --journal J [--endpoint H:P]\n"
      "  revalidate --journal J --endpoint H:P\n"
      "  revoke --journal J --endpoint H:P [--envelope N]\n"
      "  journal-scan --journal J\n"
      "\n"
      "Every rate figure is a synthetic control-plane value. No packets are involved\n"
      "and no physical shaping is performed or claimed.\n",
      stdout);
}

int run_command(int argc, char** argv) {
  Arguments args(argc, argv, 1);
  const std::string command = args.positional().empty() ? std::string() : args.positional().front();
  if (command.empty() || args.has("--help")) {
    print_usage();
    return command.empty() ? 2 : 0;
  }
  if (command == "version") {
    return command_version();
  }
  if (command == "selfcheck") {
    return command_selfcheck();
  }
  if (command == "demo") {
    return command_demo(args);
  }
  if (command == "inspect") {
    return command_inspect(args);
  }
  if (command == "apply") {
    return command_apply(args);
  }
  if (command == "recover") {
    return command_recover(args);
  }
  if (command == "revalidate") {
    return command_revalidate(args);
  }
  if (command == "revoke") {
    return command_revoke(args);
  }
  if (command == "journal-scan") {
    return command_journal_scan(args);
  }
  std::printf("rgctl: unknown command '%s'\n", command.c_str());
  print_usage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run_command(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "rgctl: unexpected failure: %s\n", error.what());
    return 4;
  }
}
