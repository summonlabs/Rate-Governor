#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

// A sink that re-enters the engine from inside the callback. If the engine ever
// invoked a sink while holding its state mutex this test would deadlock, which
// is precisely the audit it performs.
class ReentrantSink final : public IEventSink {
 public:
  void on_event(const AuditEvent& event) override {
    events.fetch_add(1, std::memory_order_relaxed);
    max_sequence.store(std::max(max_sequence.load(), event.sequence.value()),
                       std::memory_order_relaxed);
    if (event.klass == ReasonClass::UnknownAuthority) {
      unclassified.fetch_add(1, std::memory_order_relaxed);
    }
    RateGovernor* engine = engine_slot.load(std::memory_order_acquire);
    if (engine != nullptr) {
      // Read-only re-entry: legitimate, and only safe because no lock is held.
      const std::vector<EnvelopeView> views = engine->list_envelopes(4);
      observed_views.fetch_add(static_cast<u64>(views.size()), std::memory_order_relaxed);
      reentries.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::atomic<RateGovernor*> engine_slot{nullptr};
  std::atomic<u64> events{0};
  std::atomic<u64> observed_views{0};
  std::atomic<u64> reentries{0};
  std::atomic<u64> unclassified{0};
  std::atomic<u64> max_sequence{0};
};

}  // namespace

RG_TEST(concurrency, event_sinks_may_re_enter_the_engine) {
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           config.boot, config.initial_epoch, "concurrency backend");
  ReentrantSink sink;
  RateGovernor engine(config, clock, &backend, nullptr, &sink);
  sink.engine_slot.store(&engine, std::memory_order_release);
  const FixtureIds ids = install_fixture(engine, FixtureOptions{});
  BackendDescriptor descriptor = backend.describe();
  (void)engine.put_backend(descriptor);
  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "concurrency actor");

  for (int round = 0; round < 8; ++round) {
    const Result<RateEnvelopeId> opened =
        engine.open_envelope(make_request(ids, FixtureOptions{}), actor);
    RG_REQUIRE(opened.has_value());
    const Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
    RG_CHECK(applied.has_value());
    const Result<RevokeDispatch> revoked =
        engine.revoke(opened.value(), ReasonCode::EnvelopeRevokedByOperator, actor);
    RG_CHECK(revoked.has_value());
  }
  sink.engine_slot.store(nullptr, std::memory_order_release);
  RG_CHECK(sink.events.load() > 0);
  RG_CHECK(sink.observed_views.load() > 0);
  RG_CHECK_EQ(sink.reentries.load(), sink.events.load());
  RG_CHECK_EQ(sink.unclassified.load(), 0ULL);
  // Sequence numbers are assigned under the lock and must be strictly ordered.
  RG_CHECK(sink.max_sequence.load() >= sink.events.load());
}

RG_TEST(concurrency, parallel_control_plane_traffic_preserves_invariants) {
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  config.max_envelopes = 1024;
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           config.boot, config.initial_epoch, "parallel backend");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);
  const FixtureIds ids = install_fixture(engine, FixtureOptions{});
  BackendDescriptor descriptor = backend.describe();
  (void)engine.put_backend(descriptor);
  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "parallel actor");

  std::atomic<bool> invariant_violation{false};
  std::atomic<u64> operations{0};
  constexpr int kThreads = 6;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&engine, &ids, &actor, &invariant_violation, &operations, index]() {
      for (int round = 0; round < 40; ++round) {
        const Result<RateEnvelopeId> opened =
            engine.open_envelope(make_request(ids, FixtureOptions{}), actor);
        if (!opened) {
          continue;
        }
        const Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
        if (applied) {
          const Result<u64> spent = engine.consume_burst(opened.value(), 1 + (index % 7));
          (void)spent;
        }
        if (round % 3 == 0) {
          (void)engine.tick();
        }
        if (round % 5 == 0) {
          (void)engine.revoke(opened.value(), ReasonCode::EnvelopeRevokedByOperator, actor);
        }
        const Result<EnvelopeView> view = engine.inspect(opened.value());
        if (!view) {
          continue;
        }
        if (view.value().burst_tokens_available > view.value().burst_capacity) {
          invariant_violation.store(true);
        }
        if (view.value().effect_verified &&
            view.value().applied_rate_ups > view.value().effective_ceiling_ups) {
          invariant_violation.store(true);
        }
        if (view.value().state == EnvelopeState::Applied && !view.value().effect_verified) {
          invariant_violation.store(true);
        }
        operations.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  RG_CHECK(!invariant_violation.load());
  RG_CHECK(operations.load() > 0);

  // Every envelope the engine kept must still satisfy the published invariants.
  for (const EnvelopeView& view : engine.list_envelopes(1024)) {
    RG_CHECK(view.burst_tokens_available <= view.burst_capacity);
    RG_CHECK(view.effective_floor_ups <= view.effective_target_ups);
    RG_CHECK(view.effective_target_ups <= view.effective_ceiling_ups);
    if (view.state == EnvelopeState::Applied) {
      RG_CHECK(view.effect_verified);
    }
    if (view.requires_revalidation) {
      RG_CHECK(!view.effect_verified);
    }
  }
  const EngineCounters counters = engine.counters();
  RG_CHECK(counters.applies_verified + counters.applies_degraded <=
           counters.applies_dispatched);
}

RG_TEST(concurrency, shutdown_races_with_submitters_without_deadlock_or_false_success) {
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           config.boot, config.initial_epoch, "shutdown backend");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);
  const FixtureIds ids = install_fixture(engine, FixtureOptions{});
  BackendDescriptor descriptor = backend.describe();
  (void)engine.put_backend(descriptor);
  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "shutdown actor");

  std::atomic<bool> saw_inconsistent_refusal{false};
  std::atomic<u64> accepted{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&engine, &ids, &actor, &saw_inconsistent_refusal, &accepted]() {
      for (int round = 0; round < 400; ++round) {
        const Result<RateEnvelopeId> opened =
            engine.open_envelope(make_request(ids, FixtureOptions{}), actor);
        if (!opened) {
          // A refusal after shutdown must name shutdown, never something else.
          if (engine.shutting_down() && opened.code() != ErrorCode::ShuttingDown) {
            saw_inconsistent_refusal.store(true);
          }
          continue;
        }
        if (engine.shutting_down()) {
          saw_inconsistent_refusal.store(true);
        }
        const Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
        if (applied) {
          accepted.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const ShutdownReport report = engine.shutdown();
  RG_CHECK(report.work_stopped);
  for (std::thread& thread : threads) {
    thread.join();
  }
  RG_CHECK(!saw_inconsistent_refusal.load());
  // Nothing that was still in flight may be reported as verified afterwards.
  for (const EnvelopeView& view : engine.list_envelopes(1024)) {
    if (view.requires_revalidation) {
      RG_CHECK(!view.effect_verified);
    }
  }
  RG_CHECK(accepted.load() > 0);
  RG_CHECK(engine.shutting_down());
}
