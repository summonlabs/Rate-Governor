#include "test_support.hpp"

#include <memory>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

// A seeded operation-sequence driver. Every step checks the published
// invariants, so a violation is caught at the step that caused it rather than
// at the end of the run.
struct Driver {
  Driver(u64 seed, bool durable, const std::string& journal_path)
      : rng(seed), durable(durable), path(journal_path) {
    EngineConfig config;
    config.boot = WorkerBootId(11);
    config.durability = durable ? DurabilityMode::Strict : DurabilityMode::None;
    config.max_envelopes = 256;
    config.max_attempts_per_envelope = 8;
    config.attempt_deadline_ns = durable ? 0 : 500;
    config.revalidate_interval_ns = 100000;
    if (durable) {
      JournalConfig journal_config;
      journal_config.path = path;
      journal_config.max_bytes = 512 * 1024;
      JournalResult result;
      journal = Journal::open(journal_config, true, result);
    }
    backend = std::make_unique<SyntheticBackend>(FixtureIds{}.backend, Generation(1),
                                                 SyntheticDeviceConfig{}, config.boot,
                                                 config.initial_epoch, "randomized backend");
    engine = std::make_unique<RateGovernor>(config, clock, backend.get(), journal.get(), nullptr);
    ids = install_fixture(*engine, FixtureOptions{});
    BackendDescriptor descriptor = backend->describe();
    (void)engine->put_backend(descriptor);
    actor = operator_context(engine->epoch(), engine->boot(), "randomized actor");
  }

  void step() {
    const u64 choice = rng.below(13);
    switch (choice) {
      case 0:
      case 1: {
        Grant grant = base_grant();
        grant.generation = Generation(++grant_generation);
        grant.ceiling_ups = 100 + rng.below(2000);
        grant.burst_tokens = 100 + rng.below(2000);
        if (rng.chance(1, 8)) {
          grant.state = GrantState::Recalled;
        }
        (void)engine->put_grant(grant);
        break;
      }
      case 2:
      case 3: {
        Policy policy = base_policy();
        policy.generation = Generation(++policy_generation);
        policy.ceiling_ups = 100 + rng.below(2500);
        policy.floor_ups = rng.below(policy.ceiling_ups + 1);
        policy.target_ups = policy.floor_ups + rng.below(policy.ceiling_ups - policy.floor_ups + 1);
        policy.burst_tokens = rng.below(3000);
        (void)engine->put_policy(policy);
        break;
      }
      case 4: {
        Resource resource = base_resource();
        resource.generation = Generation(++resource_generation);
        resource.capacity_ups = 100 + rng.below(4000);
        (void)engine->put_resource(resource);
        break;
      }
      case 5:
      case 6: {
        const Result<RateEnvelopeId> opened =
            engine->open_envelope(make_request(ids, FixtureOptions{}), actor);
        if (opened) {
          envelopes.push_back(opened.value());
        }
        break;
      }
      case 7:
      case 8: {
        if (envelopes.empty()) {
          break;
        }
        const RateEnvelopeId target = envelopes[rng.below(envelopes.size())];
        const Result<ApplyDispatch> applied = engine->apply(target, actor);
        if (applied && applied.value().attempt.valid()) {
          live_attempts.push_back(applied.value().attempt);
        }
        break;
      }
      case 9: {
        if (envelopes.empty()) {
          break;
        }
        const RateEnvelopeId target = envelopes[rng.below(envelopes.size())];
        (void)engine->revoke(target, ReasonCode::EnvelopeRevokedByOperator, actor);
        break;
      }
      case 10: {
        if (!live_attempts.empty()) {
          const EnforcementAttemptId target = live_attempts.back();
          live_attempts.pop_back();
          if (rng.chance(1, 2)) {
            (void)engine->cancel_attempt(target, ReasonCode::AttemptCancelled, actor);
          } else {
            (void)engine->abandon_attempt(target, ReasonCode::AttemptAbandonedWorkerDeath, actor);
          }
        }
        break;
      }
      case 11: {
        clock.advance(rng.below(1000000));
        (void)engine->tick();
        break;
      }
      default: {
        if (envelopes.empty()) {
          break;
        }
        const RateEnvelopeId target = envelopes[rng.below(envelopes.size())];
        (void)engine->consume_burst(target, rng.below(600));
        if (rng.chance(1, 4)) {
          (void)engine->revalidate_all(actor);
        }
        break;
      }
    }
  }

  void check_invariants(TestContext& ctx) {
    const std::vector<EnvelopeView> views = engine->list_envelopes(256);
    for (const EnvelopeView& view : views) {
      RG_CHECK(view.burst_tokens_available <= view.burst_capacity);
      if (view.has_plan) {
        RG_CHECK(view.effective_floor_ups <= view.effective_target_ups);
        RG_CHECK(view.effective_target_ups <= view.effective_ceiling_ups);
        RG_CHECK(view.effective_ceiling_ups <= max_funded_ceiling);
      }
      if (view.state == EnvelopeState::Applied) {
        RG_CHECK(view.effect_verified);
      }
      if (view.requires_revalidation) {
        RG_CHECK(!view.effect_verified);
      }
      if (view.effect_verified) {
        RG_CHECK(view.applied_burst_tokens <= max_burst_seen);
      }
      RG_CHECK(!to_string_view(view.state).empty());
      RG_CHECK(!view.explanation.empty());
    }
    const EngineCounters counters = engine->counters();
    RG_CHECK(counters.applies_verified + counters.applies_degraded <= counters.applies_dispatched);
    RG_CHECK(counters.envelopes_opened >= views.size());
  }

  Grant base_grant() {
    Grant grant;
    grant.id = FixtureIds{}.grant;
    grant.flow = FixtureIds{}.flow;
    grant.resource = FixtureIds{}.resource;
    grant.generation = Generation(grant_generation);
    grant.ceiling_ups = 1000;
    grant.burst_tokens = 2000;
    grant.state = GrantState::Active;
    grant.provenance.source = AuthoritySource::Configuration;
    grant.provenance.detail = "randomized grant";
    return grant;
  }

  Policy base_policy() {
    Policy policy;
    policy.id = FixtureIds{}.policy;
    policy.generation = Generation(policy_generation);
    policy.floor_ups = 100;
    policy.target_ups = 600;
    policy.ceiling_ups = 1000;
    policy.burst_tokens = 2000;
    policy.refill = RefillSemantics::ContinuousTokenBucket;
    policy.state = PolicyState::Active;
    policy.provenance.source = AuthoritySource::Configuration;
    policy.provenance.detail = "randomized policy";
    return policy;
  }

  Resource base_resource() {
    Resource resource;
    resource.id = FixtureIds{}.resource;
    resource.generation = Generation(resource_generation);
    resource.capacity_ups = 4000;
    resource.state = ResourceState::Active;
    resource.provenance.source = AuthoritySource::Configuration;
    resource.provenance.detail = "randomized resource";
    return resource;
  }

  Rng rng;
  bool durable{false};
  std::string path;
  ManualClock clock;
  std::unique_ptr<Journal> journal;
  std::unique_ptr<SyntheticBackend> backend;
  std::unique_ptr<RateGovernor> engine;
  FixtureIds ids;
  ActorContext actor;
  std::vector<RateEnvelopeId> envelopes;
  std::vector<EnforcementAttemptId> live_attempts;
  u64 grant_generation{1};
  u64 policy_generation{1};
  u64 resource_generation{1};
  u64 max_funded_ceiling{4000};
  u64 max_burst_seen{8000};
};

}  // namespace

RG_TEST(randomized, seeded_operation_sequences_preserve_invariants) {
  for (u64 seed = 1; seed <= 6; ++seed) {
    RG_PHASE("seed " + std::to_string(seed));
    ScratchDir scratch("randomized_" + std::to_string(seed));
    Driver driver(seed * 0x9E3779B97F4A7C15ULL, false, scratch.file("state.rgjournal"));
    for (int step = 0; step < 220; ++step) {
      driver.step();
      if (step % 10 == 0) {
        driver.check_invariants(ctx);
      }
    }
    driver.check_invariants(ctx);
  }
}

RG_TEST(randomized, durable_sequences_survive_a_recovery) {
  for (u64 seed = 1; seed <= 3; ++seed) {
    RG_PHASE("durable seed " + std::to_string(seed));
    ScratchDir scratch("randomized_durable_" + std::to_string(seed));
    const std::string path = scratch.file("state.rgjournal");
    {
      Driver driver(seed * 0xBF58476D1CE4E5B9ULL, true, path);
      for (int step = 0; step < 120; ++step) {
        driver.step();
        driver.check_invariants(ctx);
      }
      RG_CHECK(!driver.engine->durability_degraded());
      driver.journal->close();
    }

    EngineConfig config;
    config.boot = WorkerBootId(99);
    config.journal.path = path;
    ManualClock clock(0);
    Result<RecoveredEngine> recovered = recover_engine(path, config, clock, nullptr, true);
    RG_REQUIRE(recovered.has_value());
    RG_CHECK_EQ(recovered.value().report.discarded_uncommitted_records, 0ULL);
    RateGovernor& engine = *recovered.value().engine;
    for (const EnvelopeView& view : engine.list_envelopes(256)) {
      // Nothing durable may be presented as current effect after a restart.
      RG_CHECK(!view.legally_enforceable_now);
      RG_CHECK(!view.effect_verified);
      if (view.state == EnvelopeState::Applied || view.state == EnvelopeState::Dispatching ||
          view.state == EnvelopeState::Degraded) {
        RG_CHECK(view.requires_revalidation);
      }
    }
    const JournalScanResult scan = scan_journal_file(path);
    RG_CHECK(scan.ok);
    RG_CHECK_EQ(scan.discarded_uncommitted_records, 0ULL);
  }
}
