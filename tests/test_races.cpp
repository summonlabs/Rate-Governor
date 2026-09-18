#include "test_support.hpp"

#include <functional>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

BackendDescriptor race_descriptor(BackendId id, Generation generation) {
  BackendDescriptor descriptor = fixture_backend_descriptor(id, generation);
  return descriptor;
}

// Backend that runs a caller-supplied hook while a dispatch is in flight. The
// hook runs with no engine lock held, which is exactly the window a real
// controller races with.
class RacingBackend final : public IEnforcementBackend {
 public:
  RacingBackend(BackendId id, Generation generation, WorkerBootId boot, FabricEpoch epoch)
      // In-process: this process both coordinates and enforces.
      : device_(SyntheticDeviceConfig{}, boot, epoch, boot), id_(id), generation_(generation) {
    descriptor_ = race_descriptor(id, generation);
    descriptor_.id = id;
    descriptor_.generation = generation;
    descriptor_.provenance.source = AuthoritySource::Configuration;
    descriptor_.provenance.detail = "racing test backend";
  }

  [[nodiscard]] BackendDescriptor describe() const override { return descriptor_; }

  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override {
    if (hook) {
      hook();
    }
    if (!live_) {
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Failed;
      outcome.reason = ReasonCode::BackendSessionLost;
      outcome.detail = "session lost";
      return outcome;
    }
    if (deferred) {
      // The effect happens; the completion is withheld, which is what makes a
      // delayed completion dangerous.
      (void)device_.apply(request);
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Pending;
      outcome.reason = ReasonCode::ApplyDispatched;
      outcome.detail = "racing harness deferred completion";
      outcome.epoch = device_.highest_epoch();
      outcome.boot = device_.session_boot();
      return outcome;
    }
    return device_.apply(request);
  }

  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override {
    if (!live_) {
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Failed;
      outcome.reason = ReasonCode::BackendSessionLost;
      outcome.detail = "session lost";
      return outcome;
    }
    return device_.revoke(request);
  }

  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override {
    if (verify_hook) {
      verify_hook();
    }
    if (!live_) {
      VerificationOutcome outcome;
      outcome.status = VerificationStatus::Failed;
      outcome.reason = ReasonCode::ApplyVerificationUnknown;
      outcome.detail = "session lost during readback; the effect is UNKNOWN";
      return outcome;
    }
    return device_.read_back(request);
  }

  void shutdown() noexcept override { live_ = false; }
  [[nodiscard]] WorkerBootId session_boot() const override { return device_.session_boot(); }
  [[nodiscard]] FabricEpoch session_epoch() const override { return device_.highest_epoch(); }
  [[nodiscard]] bool session_live() const override { return live_; }
  [[nodiscard]] std::string session_description() const override { return "racing synthetic"; }

  std::function<void()> hook;
  std::function<void()> verify_hook;
  bool deferred{false};
  void set_live(bool live) { live_ = live; }
  SyntheticDevice& device() { return device_; }

 private:
  SyntheticDevice device_;
  BackendDescriptor descriptor_{};
  BackendId id_{};
  Generation generation_{};
  bool live_{true};
};

struct RaceHarness {
  explicit RaceHarness(FixtureOptions options) : options(options) {
    EngineConfig config;
    config.durability = DurabilityMode::None;
    config.boot = options.boot;
    config.initial_epoch = options.epoch;
    engine = std::make_unique<RateGovernor>(config, clock, &backend, nullptr, nullptr);
    ids = install_fixture(*engine, options);
    BackendDescriptor descriptor = backend.describe();
    (void)engine->put_backend(descriptor);
    actor = fixture_actor(options);
  }

  Result<RateEnvelopeId> open() {
    return engine->open_envelope(make_request(ids, options), actor);
  }

  FixtureOptions options;
  ManualClock clock;
  RacingBackend backend{FixtureIds{}.backend, Generation(1), options.boot, options.epoch};
  std::unique_ptr<RateGovernor> engine;
  FixtureIds ids;
  ActorContext actor;
};

}  // namespace

RG_TEST(races, grant_recall_during_dispatch_cannot_publish_success) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  // The funding grant is recalled while the apply is inside the backend.
  harness.backend.hook = [&harness]() {
    const Status recalled = harness.engine->change_grant_state(
        harness.ids.grant, Generation(1), GrantState::Recalled,
        ReasonCode::EnvelopeRevokedByGrantRecall, harness.actor);
    (void)recalled;
  };
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  RG_CHECK(applied.value().resulting_state == EnvelopeState::RevokePending);

  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);
  RG_CHECK(view.value().requires_revalidation);

  // The grant generation moved, so the funded authority is gone for good.
  const Result<PlanOutcome> plan = harness.engine->explain_plan(opened.value());
  RG_REQUIRE(plan.has_value());
  RG_CHECK(!plan.value().authorized);
  RG_CHECK(plan.value().reason == ReasonCode::PlanRejectedGrantGenerationMismatch);
}

RG_TEST(races, policy_change_during_dispatch_rejects_the_completion) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  harness.backend.hook = [&harness]() {
    Policy changed;
    changed.id = harness.ids.policy;
    changed.generation = Generation(2);
    changed.floor_ups = 100;
    changed.target_ups = 400;
    changed.ceiling_ups = 500;
    changed.burst_tokens = 1000;
    changed.refill = RefillSemantics::ContinuousTokenBucket;
    changed.state = PolicyState::Active;
    changed.provenance.source = AuthoritySource::Configuration;
    changed.provenance.detail = "mid-flight policy change";
    (void)harness.engine->put_policy(changed);
  };
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  RG_CHECK(applied.value().compensating_revoke_required);

  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);
  RG_CHECK(view.value().reason == ReasonCode::EnvelopeStalePolicyGenerationChanged);
}

RG_TEST(races, epoch_advance_during_readback_fences_the_effect) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  harness.backend.verify_hook = [&harness]() {
    const ActorContext actor = operator_context(harness.engine->epoch(), harness.engine->boot(),
                                                "epoch advance hook");
    (void)harness.engine->advance_epoch(FabricEpoch(harness.options.epoch.value() + 1), actor);
  };
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);

  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(!view.value().legally_enforceable_now);
  RG_CHECK_EQ(harness.engine->epoch().value(), harness.options.epoch.value() + 1);
}

RG_TEST(races, worker_death_during_readback_leaves_the_effect_unknown) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.verify_hook = [&harness]() { harness.backend.set_live(false); };
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Degraded);
  RG_CHECK(view.value().reason == ReasonCode::ApplyVerificationUnknown);
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(view.value().action == EnvelopeAction::Revalidate);
}

RG_TEST(races, abandoning_an_attempt_requires_revalidation) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.hook = []() {};
  // A deferred completion is simulated by cancelling through the abandon path,
  // which is what a lost worker session looks like to the engine.
  harness.backend.deferred = true;
  const Result<ApplyDispatch> dispatch = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(dispatch.has_value());
  const Status abandoned = harness.engine->abandon_attempt(
      dispatch.value().attempt, ReasonCode::AttemptAbandonedWorkerDeath, harness.actor);
  RG_REQUIRE(abandoned.has_value());
  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(view.value().state == EnvelopeState::Degraded);
}

RG_TEST(races, clock_regression_and_tick_overflow_are_survivable) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine->apply(opened.value(), harness.actor).has_value());

  harness.clock.set(1000000000ULL);
  TickReport tick = harness.engine->tick();
  RG_CHECK_EQ(tick.clock_regressions, 0ULL);

  // The clock jumps backwards: no tokens may be minted, and the anomaly is
  // reported rather than absorbed.
  harness.clock.set(1);
  tick = harness.engine->tick();
  // Both the tick itself and the bucket refill observe the regression.
  RG_CHECK(tick.clock_regressions >= 1ULL);
  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().burst_tokens_available <= view.value().burst_capacity);

  // A huge forward jump saturates the bucket instead of wrapping it.
  harness.clock.set(kU64Max - 1);
  tick = harness.engine->tick();
  const Result<EnvelopeView> saturated = harness.engine->inspect(opened.value());
  RG_REQUIRE(saturated.has_value());
  RG_CHECK_EQ(saturated.value().burst_tokens_available, saturated.value().burst_capacity);
  RG_CHECK(saturated.value().burst_tokens_available <= saturated.value().burst_capacity);
}

RG_TEST(races, malformed_and_contradictory_completions_are_refused) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.deferred = true;
  const Result<ApplyDispatch> dispatch = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(dispatch.has_value());

  // Foreign boot id: the completion did not come from the incarnation the
  // attempt was dispatched to.
  CompletionReport foreign;
  foreign.status = DispatchStatus::Completed;
  foreign.acknowledged = true;
  foreign.epoch = harness.options.epoch;
  foreign.boot = WorkerBootId(999999);
  Result<CompletionOutcome> outcome =
      harness.engine->complete_apply(dispatch.value().attempt, foreign, harness.actor);
  RG_REQUIRE(outcome.has_value());
  RG_CHECK(outcome.value().rejected_late);
  RG_CHECK(outcome.value().reason == ReasonCode::AttemptFencedByBoot);
  RG_CHECK(!outcome.value().verified);

  // The attempt is now terminal, so nothing can revive it.
  CompletionReport correct;
  correct.status = DispatchStatus::Completed;
  correct.acknowledged = true;
  correct.epoch = harness.options.epoch;
  correct.boot = harness.backend.session_boot();
  outcome = harness.engine->complete_apply(dispatch.value().attempt, correct, harness.actor);
  RG_REQUIRE(outcome.has_value());
  RG_CHECK(!outcome.value().verified);
  RG_CHECK(!outcome.value().accepted);

  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
}

RG_TEST(races, stale_actor_incarnations_are_fenced) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  ActorContext stale_epoch = harness.actor;
  stale_epoch.epoch = FabricEpoch(harness.options.epoch.value() - 1);
  const Result<PlanOutcome> plan = harness.engine->authorize(opened.value(), stale_epoch);
  RG_CHECK(!plan.has_value());
  RG_CHECK(plan.reason() == ReasonCode::AttemptFencedByEpoch);

  ActorContext wrong_boot = harness.actor;
  wrong_boot.boot = WorkerBootId(harness.options.boot.value() + 1);
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), wrong_boot);
  RG_CHECK(!applied.has_value());
  RG_CHECK(applied.reason() == ReasonCode::AttemptFencedByBoot);
  RG_CHECK(harness.engine->counters().fencing_rejections >= 2);

  ActorContext unknown_authority = harness.actor;
  unknown_authority.provenance.source = AuthoritySource::Unknown;
  const Result<ApplyDispatch> unknown = harness.engine->apply(opened.value(), unknown_authority);
  RG_CHECK(!unknown.has_value());
  RG_CHECK(unknown.reason() == ReasonCode::UnknownAuthority);
}

RG_TEST(races, expired_grant_mid_flight_is_rejected) {
  RaceHarness harness(FixtureOptions{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.hook = [&harness]() {
    Grant expiring;
    expiring.id = harness.ids.grant;
    expiring.flow = harness.ids.flow;
    expiring.resource = harness.ids.resource;
    expiring.generation = Generation(2);
    expiring.ceiling_ups = 1000;
    expiring.burst_tokens = 2000;
    expiring.window.not_before_ns = 0;
    expiring.window.invalid_after_ns = 1;
    expiring.state = GrantState::Active;
    expiring.provenance.source = AuthoritySource::Configuration;
    expiring.provenance.detail = "expiring grant";
    (void)harness.engine->put_grant(expiring);
    harness.clock.set(1000);
  };
  const Result<ApplyDispatch> applied = harness.engine->apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  const Result<EnvelopeView> view = harness.engine->inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(!view.value().legally_enforceable_now);
}
