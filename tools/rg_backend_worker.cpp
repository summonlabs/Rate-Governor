// rg_backend_worker - the SYNTHETIC enforcement worker process.
//
// This program owns an in-memory enforcement table and serves framed requests
// over a real loopback socket. It is not a NIC, a shaper or a policer: it makes
// enforcement *semantics* executable across a real process boundary. Every
// claim it reports is labelled synthetic.
//
// Two transports modes exist:
//   --connect H:P  the worker dials a coordinator that is already listening.
//   --listen  H:P  the worker listens and serves concurrent sessions from one
//                  shared device, so several coordinator incarnations can be
//                  fenced against the same enforcement state.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cli_common.hpp"
#include "rate_governor/rate_governor.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

rate_governor::u64 current_process_id() {
#if defined(_WIN32)
  return static_cast<rate_governor::u64>(::_getpid());
#else
  return static_cast<rate_governor::u64>(::getpid());
#endif
}

void print_usage() {
  std::fputs(
      "rg_backend_worker - SYNTHETIC rate-envelope enforcement worker\n"
      "\n"
      "usage: rg_backend_worker (--connect host:port | --listen host:port) [options]\n"
      "\n"
      "  --connect H:P              dial a coordinator that is already listening\n"
      "  --listen H:P               serve sessions (port 0 picks an ephemeral port)\n"
      "  --epoch N                  highest fabric epoch already observed (default 0)\n"
      "  --boot N                   override the process boot id (default: derived)\n"
      "  --backend-id N             backend identity reported in the handshake (default 1)\n"
      "  --backend-gen N            backend generation (default 1)\n"
      "  --threads N                per-session worker pool threads (default 2)\n"
      "  --queue N                  bounded request queue capacity (default 64)\n"
      "  --max-sessions N           concurrent session bound in listen mode (default 8)\n"
      "  --envelope-limit N         maximum envelopes the device will hold (default 4096)\n"
      "  --max-rate N               declared device rate ceiling, 0 = undeclared (default 0)\n"
      "  --max-burst N              declared device burst ceiling, 0 = undeclared (default 0)\n"
      "  --fail-every-nth-apply N   deterministic failure injection (default 0 = never)\n"
      "  --refuse-every-nth-verify N\n"
      "  --underreport-every-nth-verify N\n"
      "  --overreport-every-nth-verify N\n"
      "  --apply-spins N            deterministic busy work per apply (widens race windows)\n"
      "  --refuse-all-applies       refuse every apply\n"
      "  --refuse-all-verifies      fail every readback\n"
      "  --verbose                  log receive failures to stderr\n"
      "\n"
      "Exit codes: 0 clean stop, 2 usage error, 3 bind/connect failure, 4 unexpected failure.\n"
      "This worker performs no physical shaping and makes no packet-rate claim.\n",
      stdout);
}

}  // namespace

int run_worker(int argc, char** argv) {
  using namespace rate_governor;
  using rate_governor::tools::Arguments;

  Arguments args(argc, argv, 1);
  const std::string connect_text = args.get("--connect");
  const std::string listen_text = args.get("--listen");
  if (args.has("--help") || connect_text.empty() == listen_text.empty()) {
    print_usage();
    return args.has("--help") ? 0 : 2;
  }

  WorkerSessionConfig config;
  config.backend = BackendId(args.get_u64("--backend-id", 1));
  config.backend_generation = Generation(args.get_u64("--backend-gen", 1));
  config.starting_epoch = FabricEpoch(args.get_u64("--epoch", 0));
  const u64 derived_boot = mix64(current_process_id()) ^ mix64(SteadyClock().NowNs());
  config.boot = WorkerBootId(args.get_u64("--boot", derived_boot == 0 ? 1 : derived_boot));
  config.threads = static_cast<std::uint32_t>(args.get_u64("--threads", 2));
  if (config.threads == 0) {
    config.threads = 1;
  }
  config.queue_capacity = static_cast<usize>(args.get_u64("--queue", 64));
  if (config.queue_capacity == 0) {
    config.queue_capacity = 1;
  }
  config.verbose = args.has("--verbose");
  config.device.envelope_limit = args.get_u64("--envelope-limit", 4096);
  config.device.max_rate_ups = args.get_u64("--max-rate", 0);
  config.device.max_burst_tokens = args.get_u64("--max-burst", 0);
  config.device.fail_every_nth_apply = args.get_u64("--fail-every-nth-apply", 0);
  config.device.refuse_every_nth_verify = args.get_u64("--refuse-every-nth-verify", 0);
  config.device.underreport_every_nth_verify = args.get_u64("--underreport-every-nth-verify", 0);
  config.device.overreport_every_nth_verify = args.get_u64("--overreport-every-nth-verify", 0);
  config.device.apply_spins = args.get_u64("--apply-spins", 0);
  config.device.refuse_all_applies = args.has("--refuse-all-applies");
  config.device.refuse_all_verifies = args.has("--refuse-all-verifies");
  config.build = std::string(kProductName) + " " + std::string(kVersionString) +
                 " synthetic worker (no physical shaping)";

  ensure_socket_runtime();
  std::string error;

  if (!connect_text.empty()) {
    Endpoint endpoint;
    if (!Endpoint::parse(connect_text, endpoint)) {
      std::fputs("rg_backend_worker: --connect must be host:port\n", stderr);
      return 2;
    }
    Socket socket;
    if (!Socket::connect_to(endpoint, socket, error)) {
      std::fprintf(stderr, "rg_backend_worker: cannot connect to %s: %s\n",
                   endpoint.to_string().c_str(), error.c_str());
      return 3;
    }
    WorkerSession session(config, std::move(socket));
    session.run();
    const WorkerSessionAccounting accounting = session.accounting();
    std::printf(
        "{\"worker_boot\":%llu,\"sessions\":1,\"requests_served\":%llu,\"applies\":%llu,"
        "\"revokes\":%llu,\"verifications\":%llu,\"rejections\":%llu,"
        "\"malformed_frames\":%llu,\"fenced_stale_epoch\":%llu,\"fenced_stale_boot\":%llu,"
        "\"queue_rejections\":%llu,\"queue_high_water\":%llu,\"stopped\":%s}\n",
        static_cast<unsigned long long>(config.boot.value()),
        static_cast<unsigned long long>(accounting.requests_served),
        static_cast<unsigned long long>(accounting.applies_applied),
        static_cast<unsigned long long>(accounting.revokes_applied),
        static_cast<unsigned long long>(accounting.verifications),
        static_cast<unsigned long long>(accounting.rejections),
        static_cast<unsigned long long>(accounting.malformed_frames),
        static_cast<unsigned long long>(accounting.fenced_stale_epoch),
        static_cast<unsigned long long>(accounting.fenced_stale_boot),
        static_cast<unsigned long long>(accounting.queue_rejections),
        static_cast<unsigned long long>(accounting.queue_high_water),
        session.stopped() ? "true" : "false");
    return 0;
  }

  Endpoint endpoint;
  if (!Endpoint::parse(listen_text, endpoint)) {
    std::fputs("rg_backend_worker: --listen must be host:port\n", stderr);
    return 2;
  }
  std::unique_ptr<Listener> listener = Listener::bind_loopback(endpoint.port, error);
  if (listener == nullptr) {
    std::fprintf(stderr, "rg_backend_worker: cannot listen on %s: %s\n",
                 endpoint.to_string().c_str(), error.c_str());
    return 3;
  }
  std::printf("rg_backend_worker: listening on 127.0.0.1:%u boot=%s epoch=%s (SYNTHETIC)\n",
              static_cast<unsigned>(listener->port()), config.boot.to_string().c_str(),
              config.starting_epoch.to_string().c_str());
  std::fflush(stdout);

  auto device = std::make_shared<SyntheticDevice>(config.device, config.boot,
                                                  config.starting_epoch);
  const usize max_sessions = static_cast<usize>(args.get_u64("--max-sessions", 8));
  std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> sessions;
  u64 served = 0;
  // A session that served an explicit shutdown request ends the whole worker:
  // the listener is closed and the accept loop exits. Exactly one thread does
  // the closing, so the socket handle is never closed twice.
  auto stop_listening = std::make_shared<std::atomic<bool>>(false);
  // Per-connection sessions have their own counters; a listener aggregates them
  // so an operator can see what the whole worker served.
  struct SessionTotals {
    std::mutex mutex;
    WorkerSessionAccounting accounting{};
  };
  auto totals = std::make_shared<SessionTotals>();
  // Live sessions are registered so that stopping the listener can signal every
  // session instead of joining a thread that is still blocked on a healthy
  // socket. Pointers are only valid while the owning thread holds the lock.
  struct SessionRegistry {
    std::mutex mutex;
    std::vector<WorkerSession*> live;
  };
  auto registry = std::make_shared<SessionRegistry>();

  while (true) {
    // Reap sessions that already finished so a long-lived worker cannot grow
    // its thread bookkeeping without bound.
    for (usize index = 0; index < sessions.size();) {
      if (sessions[index].second->load(std::memory_order_acquire)) {
        sessions[index].first.join();
        sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        ++index;
      }
    }
    if (sessions.size() >= max_sessions) {
      std::fputs("rg_backend_worker: session bound reached; refusing a new connection\n", stderr);
      Socket rejected;
      if (listener->accept(rejected, error)) {
        rejected.close();
      }
      continue;
    }
    Socket socket;
    if (!listener->accept(socket, error)) {
      if (stop_listening->load(std::memory_order_acquire)) {
        break;
      }
      std::fprintf(stderr, "rg_backend_worker: accept failed: %s\n", error.c_str());
      break;
    }
    if (stop_listening->load(std::memory_order_acquire)) {
      socket.close();
      break;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    sessions.emplace_back(
        std::thread([config, device, sock = std::move(socket), finished, stop_listening, endpoint,
                     totals, registry]() mutable {
          WorkerSession session(config, std::move(sock), device);
          {
            std::lock_guard<std::mutex> guard(registry->mutex);
            registry->live.push_back(&session);
          }
          session.run();
          {
            std::lock_guard<std::mutex> guard(registry->mutex);
            std::vector<WorkerSession*>& live = registry->live;
            live.erase(std::remove(live.begin(), live.end(), &session), live.end());
          }
          const WorkerSessionAccounting served = session.accounting();
          {
            std::lock_guard<std::mutex> guard(totals->mutex);
            totals->accounting.requests_served += served.requests_served;
            totals->accounting.applies_applied += served.applies_applied;
            totals->accounting.revokes_applied += served.revokes_applied;
            totals->accounting.verifications += served.verifications;
            totals->accounting.rejections += served.rejections;
            totals->accounting.malformed_frames += served.malformed_frames;
            totals->accounting.fenced_stale_epoch += served.fenced_stale_epoch;
            totals->accounting.fenced_stale_boot += served.fenced_stale_boot;
            totals->accounting.queue_rejections += served.queue_rejections;
            totals->accounting.idle_noop_requests += served.idle_noop_requests;
            if (served.queue_high_water > totals->accounting.queue_high_water) {
              totals->accounting.queue_high_water = served.queue_high_water;
            }
          }
          finished->store(true, std::memory_order_release);
          if (config.verbose) {
            std::fprintf(stderr,
                         "[rate-governor-worker] session ended shutdown_requested=%s stopping=%s\n",
                         session.shutdown_requested() ? "true" : "false",
                         session.stopped() ? "true" : "false");
          }
          if (session.shutdown_requested() && !stop_listening->exchange(true)) {
            // Wake the accept loop with a throwaway connection. Closing the
            // listening socket from another thread does not reliably cancel a
            // blocking accept(), so the loop is woken with real traffic and
            // then exits of its own accord.
            Socket wake;
            std::string wake_error;
            const bool woke = Socket::connect_to(endpoint, wake, wake_error);
            if (config.verbose) {
              std::fprintf(stderr, "[rate-governor-worker] wake-up connection sent=%s%s\n",
                           woke ? "true" : "false",
                           woke ? "" : (" error=" + wake_error).c_str());
            }
            wake.close();
          }
        }),
        finished);
    ++served;
  }
  // Stopping the listener stops the sessions it is still holding open: signal
  // every live session first, then join. Signalling never blocks on the socket,
  // so no session can be joined while it still waits for traffic.
  {
    std::lock_guard<std::mutex> guard(registry->mutex);
    if (config.verbose) {
      std::fprintf(stderr, "[rate-governor-worker] accept loop ended; stopping %zu session(s)\n",
                   registry->live.size());
    }
    for (WorkerSession* session : registry->live) {
      if (config.verbose) {
        std::fprintf(stderr, "[rate-governor-worker] requesting session stop\n");
      }
      session->request_stop("listener is stopping");
      if (config.verbose) {
        std::fprintf(stderr, "[rate-governor-worker] session stop requested\n");
      }
    }
  }
  if (config.verbose) {
    std::fprintf(stderr, "[rate-governor-worker] joining %zu session thread(s)\n", sessions.size());
  }
  for (auto& entry : sessions) {
    entry.first.join();
  }
  if (config.verbose) {
    std::fprintf(stderr, "[rate-governor-worker] all session threads joined\n");
  }
  const DeviceAccounting accounting = device->accounting();
  WorkerSessionAccounting session_totals;
  {
    std::lock_guard<std::mutex> guard(totals->mutex);
    session_totals = totals->accounting;
  }
  std::printf(
      "{\"worker_boot\":%llu,\"sessions\":%llu,\"applies\":%llu,\"revokes\":%llu,"
      "\"verifications\":%llu,\"refusals\":%llu,\"fenced_stale_epoch\":%llu,"
      "\"fenced_stale_boot\":%llu,\"table_clears\":%llu,\"active_envelopes\":%llu,"
      "\"malformed_frames\":%llu,\"requests_served\":%llu}\n",
      static_cast<unsigned long long>(config.boot.value()),
      static_cast<unsigned long long>(served),
      static_cast<unsigned long long>(accounting.applies),
      static_cast<unsigned long long>(accounting.revokes),
      static_cast<unsigned long long>(accounting.verifications),
      static_cast<unsigned long long>(accounting.refusals),
      static_cast<unsigned long long>(accounting.fenced_stale_epoch),
      static_cast<unsigned long long>(accounting.fenced_stale_boot),
      static_cast<unsigned long long>(accounting.table_clears),
      static_cast<unsigned long long>(device->entry_count()),
      static_cast<unsigned long long>(session_totals.malformed_frames),
      static_cast<unsigned long long>(session_totals.requests_served));
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run_worker(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "rg_backend_worker: unexpected failure: %s\n", error.what());
    return 4;
  }
}
