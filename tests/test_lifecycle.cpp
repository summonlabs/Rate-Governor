#include "test_support.hpp"

#include <functional>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

// A synthetic backend with programmable behaviour. The device underneath is
// the shipped synthetic table; the switches only change what the backend
// *reports*, which is exactly what the engine must not blindly trust.
class ScriptedBackend final : public IEnforcementBackend {
 public:
  ScriptedBackend(BackendId id, Generation generation, SyntheticDeviceConfig config,
                  WorkerBootId boot, FabricEpoch epoch)
      // In-process: this process both coordinates and enforces, so it admits
      // its own incarnation.
      : device_(config, boot, epoch, boot),
        id_(id),
        generation_(generation),
        boot_(boot),
        epoch_(epoch) {
    descriptor_.id = id;
    descriptor_.generation = generation;
    descriptor_.kind = BackendKind::Synthetic;
    descriptor_.capabilities =
        kBackendCapApply | kBackendCapRevoke | kBackendCapVerify | kBackendCapBurst;
    descriptor_.verification = VerificationMode::PostApplyReadback;
    descriptor_.max_rate_ups = config.max_rate_ups;
    descriptor_.max_burst_tokens = config.max_burst_tokens;
    descriptor_.label = "scripted synthetic device";
    descriptor_.provenance.source = AuthoritySource::Configuration;
    descriptor_.provenance.detail = "scripted test backend";
  }

  [[nodiscard]] BackendDescriptor describe() const override { return descriptor_; }

  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override {
    ++apply_calls;
    if (during_apply) {
      during_apply();
    }
    if (refuse_apply) {
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Rejected;
      outcome.reason = ReasonCode::ApplyBackendFailed;
      outcome.detail = "scripted refusal";
      outcome.epoch = epoch_;
      outcome.boot = boot_;
      return outcome;
    }
    if (defer_completion) {
      // The effect really happens; only the *completion* is deferred. That is
      // what makes a late completion dangerous rather than harmless.
      (void)device_.apply(request);
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Pending;
      outcome.reason = ReasonCode::ApplyDispatched;
      outcome.detail = "scripted deferred completion";
      outcome.epoch = epoch_;
      outcome.boot = boot_;
      return outcome;
    }
    DispatchOutcome outcome = device_.apply(request);
    if (outcome.completed()) {
      if (under_report_to != 0) {
        outcome.applied_rate_ups = under_report_to;
      }
      if (over_report_by != 0) {
        outcome.applied_rate_ups = saturating_add(outcome.applied_rate_ups, over_report_by);
      }
    }
    return outcome;
  }

  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override {
    if (refuse_revoke) {
      DispatchOutcome outcome;
      outcome.status = DispatchStatus::Rejected;
      outcome.reason = ReasonCode::RevokeBackendFailed;
      outcome.detail = "scripted refusal";
      outcome.epoch = epoch_;
      outcome.boot = boot_;
      return outcome;
    }
    return device_.revoke(request);
  }

  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override {
    ++verify_calls;
    if (during_verify) {
      during_verify();
    }
    if (refuse_verify) {
      VerificationOutcome outcome;
      outcome.status = VerificationStatus::Failed;
      outcome.reason = ReasonCode::ApplyVerificationUnknown;
      outcome.detail = "scripted readback failure";
      return outcome;
    }
    if (report_unsupported) {
      VerificationOutcome outcome;
      outcome.status = VerificationStatus::Unsupported;
      outcome.reason = ReasonCode::ApplyVerificationUnsupported;
      outcome.detail = "scripted unsupported readback";
      return outcome;
    }
    VerificationOutcome outcome = device_.read_back(request);
    if (outcome.status == VerificationStatus::Confirmed && outcome.observed_present) {
      // The readback is where under- and over-delivery must appear: the
      // acknowledgement is not evidence.
      if (under_report_to != 0) {
        outcome.observed_rate_ups = under_report_to;
        outcome.observed_burst_tokens = 0;
        outcome.reason = ReasonCode::ApplyUnderDelivered;
      } else if (over_report_by != 0) {
        outcome.observed_rate_ups = saturating_add(outcome.observed_rate_ups, over_report_by);
        outcome.reason = ReasonCode::ApplyOverDeliveredRevoking;
      }
      if (report_present) {
        outcome.observed_rate_ups = report_rate;
        outcome.observed_burst_tokens = report_burst;
        outcome.reason = ReasonCode::ApplyVerified;
      }
      if (report_absent) {
        outcome.observed_present = false;
        outcome.observed_rate_ups = 0;
        outcome.observed_burst_tokens = 0;
      }
    }
    return outcome;
  }

  void shutdown() noexcept override {
    ++shutdown_calls;
    live_ = false;
  }

  [[nodiscard]] WorkerBootId session_boot() const override { return device_.session_boot(); }
  [[nodiscard]] FabricEpoch session_epoch() const override { return device_.highest_epoch(); }
  [[nodiscard]] bool session_live() const override { return live_; }
  [[nodiscard]] std::string session_description() const override { return "scripted synthetic"; }

  SyntheticDevice& device() { return device_; }

  // Scripted behaviours.
  std::function<void()> during_apply;
  std::function<void()> during_verify;
  bool refuse_apply{false};
  bool refuse_revoke{false};
  bool refuse_verify{false};
  bool report_unsupported{false};
  bool defer_completion{false};
  bool report_present{false};
  bool report_absent{false};
  u64 report_rate{0};
  u64 report_burst{0};
  u64 under_report_to{0};
  u64 over_report_by{0};
  u64 apply_calls{0};
  u64 verify_calls{0};
  u64 shutdown_calls{0};
  void set_live(bool live) { live_ = live; }

 private:
  SyntheticDevice device_;
  BackendDescriptor descriptor_{};
  BackendId id_{};
  Generation generation_{};
  WorkerBootId boot_{};
  FabricEpoch epoch_{};
  bool live_{true};
};

// Everything a lifecycle test needs, wired consistently.
struct Harness {
  Harness(const FixtureOptions& options, SyntheticDeviceConfig device_config,
          u64 revalidate_interval_ns = 0)
      : options(options),
        backend(FixtureIds{}.backend, Generation(1), device_config, options.boot, options.epoch),
        engine(make_config(options, revalidate_interval_ns), clock, &backend, nullptr, nullptr) {
    const FixtureIds fixture = install_fixture(engine, options);
    ids = fixture;
    BackendDescriptor descriptor = backend.describe();
    (void)engine.put_backend(descriptor);
    actor = fixture_actor(options);
  }

  static EngineConfig make_config(const FixtureOptions& options, u64 revalidate_interval_ns = 0) {
    EngineConfig config;
    config.durability = DurabilityMode::None;
    config.boot = options.boot;
    config.initial_epoch = options.epoch;
    config.attempt_deadline_ns = 0;
    config.revalidate_interval_ns = revalidate_interval_ns;
    return config;
  }

  Result<RateEnvelopeId> open() {
    return engine.open_envelope(make_request(ids, options), actor);
  }

  FixtureOptions options;
  ManualClock clock;
  ScriptedBackend backend;
  RateGovernor engine;
  FixtureIds ids;
  ActorContext actor;
};

}  // namespace

RG_TEST(lifecycle, verified_apply_publishes_applied_with_the_funded_envelope) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  if (!opened.has_value()) {
    RG_NOTE("open failed: " + opened.message() + " code=" +
            std::to_string(static_cast<int>(opened.code())) + " reason=" +
            std::string(to_string_view(opened.reason())) + " epoch=" +
            harness.engine.epoch().to_string() + " boot=" + harness.engine.boot().to_string() +
            " storage=" + std::to_string(harness.engine.storage_stats().grants));
  }
  RG_REQUIRE(opened.has_value());
  const RateEnvelopeId envelope = opened.value();

  const Result<PlanOutcome> plan = harness.engine.explain_plan(envelope);
  RG_REQUIRE(plan.has_value());
  RG_CHECK(plan.value().authorized);
  RG_CHECK_EQ(plan.value().plan.ceiling_ups(), 1000ULL);

  const Result<ApplyDispatch> applied = harness.engine.apply(envelope, harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(applied.value().verified);
  RG_CHECK(applied.value().verification_performed);
  RG_CHECK(applied.value().resulting_state == EnvelopeState::Applied);

  const Result<EnvelopeView> view = harness.engine.inspect(envelope);
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Applied);
  RG_CHECK(view.value().effect_verified);
  RG_CHECK(view.value().legally_enforceable_now);
  RG_CHECK(view.value().action == EnvelopeAction::None);
  RG_CHECK_EQ(view.value().applied_rate_ups, 1000ULL);
  RG_CHECK_EQ(view.value().burst_capacity, 2000ULL);
  RG_CHECK_EQ(view.value().burst_tokens_available, 2000ULL);
  RG_CHECK_EQ(view.value().attempts_issued, 1ULL);
  RG_CHECK_EQ(view.value().completions_applied, 1ULL);
  RG_CHECK(!describe(view.value()).empty());
  RG_CHECK(!view.value().explanation.empty());
}

RG_TEST(lifecycle, acknowledgement_alone_never_becomes_applied) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  const RateEnvelopeId envelope = opened.value();

  // The backend acknowledges but the readback cannot confirm anything.
  harness.backend.refuse_verify = true;
  const Result<ApplyDispatch> applied = harness.engine.apply(envelope, harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  RG_CHECK(applied.value().resulting_state == EnvelopeState::Degraded);

  const Result<EnvelopeView> view = harness.engine.inspect(envelope);
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(view.value().action == EnvelopeAction::Revalidate);

  // An explicitly unsupported readback behaves the same way: no APPLIED claim.
  Harness second(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> second_envelope = second.open();
  RG_REQUIRE(second_envelope.has_value());
  second.backend.report_unsupported = true;
  const Result<ApplyDispatch> second_applied =
      second.engine.apply(second_envelope.value(), second.actor);
  RG_REQUIRE(second_applied.has_value());
  RG_CHECK(!second_applied.value().verified);
  RG_CHECK(second_applied.value().resulting_state == EnvelopeState::Degraded);
  const Result<EnvelopeView> second_view = second.engine.inspect(second_envelope.value());
  RG_REQUIRE(second_view.has_value());
  RG_CHECK(second_view.value().reason == ReasonCode::ApplyVerificationUnsupported);
}

RG_TEST(lifecycle, a_backend_that_does_not_hold_the_envelope_is_degraded) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.report_absent = true;
  const Result<ApplyDispatch> applied = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(!applied.value().verified);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Degraded);
  RG_CHECK(view.value().reason == ReasonCode::ApplyVerificationMismatch);
  RG_CHECK(!view.value().effect_verified);
}

RG_TEST(lifecycle, under_delivery_below_the_floor_requires_a_compensating_revoke) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.under_report_to = 10;  // far below the 100 ups floor
  const Result<ApplyDispatch> applied = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(applied.value().compensating_revoke_required);
  RG_CHECK(applied.value().resulting_state == EnvelopeState::RevokePending);

  Harness mild(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> mild_envelope = mild.open();
  RG_REQUIRE(mild_envelope.has_value());
  mild.backend.under_report_to = 500;  // below the ceiling, above the floor
  const Result<ApplyDispatch> mild_applied =
      mild.engine.apply(mild_envelope.value(), mild.actor);
  RG_REQUIRE(mild_applied.has_value());
  RG_CHECK(mild_applied.value().resulting_state == EnvelopeState::Degraded);
  const Result<EnvelopeView> mild_view = mild.engine.inspect(mild_envelope.value());
  RG_REQUIRE(mild_view.has_value());
  RG_CHECK(mild_view.value().reason == ReasonCode::ApplyUnderDelivered);
  RG_CHECK_EQ(mild_view.value().applied_rate_ups, 500ULL);
  RG_CHECK(mild_view.value().action == EnvelopeAction::RetryApply);
}

RG_TEST(lifecycle, over_delivery_beyond_the_authorized_ceiling_is_revoked) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.over_report_by = 1;
  const Result<ApplyDispatch> applied = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(applied.has_value());
  RG_CHECK(applied.value().compensating_revoke_required);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);
  RG_CHECK(view.value().reason == ReasonCode::ApplyOverDeliveredRevoking);
  RG_CHECK(view.value().action == EnvelopeAction::Revoke);
}

RG_TEST(lifecycle, revoke_is_verified_and_idempotent) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());

  const Result<RevokeDispatch> revoked = harness.engine.revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByOperator, harness.actor);
  RG_REQUIRE(revoked.has_value());
  RG_CHECK(revoked.value().verified);
  RG_CHECK(revoked.value().resulting_state == EnvelopeState::Revoked);

  const Result<RevokeDispatch> again = harness.engine.revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByOperator, harness.actor);
  RG_REQUIRE(again.has_value());
  RG_CHECK(again.value().duplicate);
  RG_CHECK(again.value().resulting_state == EnvelopeState::Revoked);

  // A revoked envelope cannot be re-applied: the binding must be reopened.
  const Result<ApplyDispatch> reapply = harness.engine.apply(opened.value(), harness.actor);
  RG_CHECK(!reapply.has_value());
  RG_CHECK(reapply.code() == ErrorCode::Conflict);

  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK_EQ(view.value().applied_rate_ups, 0ULL);
  RG_CHECK_EQ(view.value().burst_tokens_available, 0ULL);
}

RG_TEST(lifecycle, a_refused_revoke_stays_revoke_pending) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());
  harness.backend.refuse_revoke = true;
  const Result<RevokeDispatch> revoked = harness.engine.revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByOperator, harness.actor);
  RG_REQUIRE(revoked.has_value());
  RG_CHECK(!revoked.value().verified);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);
  RG_CHECK(view.value().requires_revalidation);
}

RG_TEST(lifecycle, duplicate_apply_while_in_flight_is_not_dispatched_twice) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.defer_completion = true;

  const Result<ApplyDispatch> first = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(first.has_value());
  RG_CHECK(first.value().awaiting_completion);
  RG_CHECK_EQ(harness.backend.apply_calls, 1ULL);

  const Result<ApplyDispatch> second = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(second.has_value());
  RG_CHECK(second.value().duplicate);
  RG_CHECK(second.value().reason == ReasonCode::AttemptIdempotentReplay);
  RG_CHECK_EQ(harness.backend.apply_calls, 1ULL);
  RG_CHECK_EQ(second.value().attempt.value(), first.value().attempt.value());

  // A late completion for the deferred attempt is accepted once and only once.
  CompletionReport report;
  report.status = DispatchStatus::Completed;
  report.acknowledged = true;
  report.acknowledged_rate_ups = 1000;
  report.acknowledged_burst_tokens = 2000;
  report.epoch = harness.options.epoch;
  report.boot = harness.backend.session_boot();
  const Result<CompletionOutcome> completed =
      harness.engine.complete_apply(first.value().attempt, report, harness.actor);
  RG_REQUIRE(completed.has_value());
  RG_CHECK(completed.value().accepted);
  RG_CHECK(completed.value().verified);
  RG_CHECK(completed.value().resulting_state == EnvelopeState::Applied);

  const Result<CompletionOutcome> duplicate =
      harness.engine.complete_apply(first.value().attempt, report, harness.actor);
  RG_REQUIRE(duplicate.has_value());
  RG_CHECK(duplicate.value().duplicate);
  RG_CHECK(!duplicate.value().accepted);
  RG_CHECK(duplicate.value().reason == ReasonCode::AttemptDuplicateIgnored);
}

RG_TEST(lifecycle, cancelled_attempts_can_never_publish_success) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.defer_completion = true;
  const Result<ApplyDispatch> dispatch = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(dispatch.has_value());

  const Status cancelled = harness.engine.cancel_attempt(
      dispatch.value().attempt, ReasonCode::AttemptCancelled, harness.actor);
  RG_REQUIRE(cancelled.has_value());

  CompletionReport report;
  report.status = DispatchStatus::Completed;
  report.acknowledged = true;
  report.acknowledged_rate_ups = 1000;
  report.acknowledged_burst_tokens = 2000;
  report.epoch = harness.options.epoch;
  report.boot = harness.backend.session_boot();
  const Result<CompletionOutcome> completed =
      harness.engine.complete_apply(dispatch.value().attempt, report, harness.actor);
  RG_REQUIRE(completed.has_value());
  RG_CHECK(completed.value().rejected_late);
  RG_CHECK(!completed.value().verified);
  RG_CHECK(!completed.value().accepted);
  // The backend may really hold the envelope now, so a compensating revoke is
  // required rather than pretending nothing happened.
  RG_CHECK(completed.value().compensating_revoke_required);
  RG_CHECK(completed.value().resulting_state == EnvelopeState::RevokePending);

  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);
}

RG_TEST(lifecycle, burst_can_only_be_spent_on_a_verified_effect) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  const Result<u64> early = harness.engine.consume_burst(opened.value(), 10);
  RG_CHECK(!early.has_value());
  RG_CHECK(early.code() == ErrorCode::Denied);

  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());
  const Result<u64> spent = harness.engine.consume_burst(opened.value(), 1500);
  RG_REQUIRE(spent.has_value());
  RG_CHECK_EQ(spent.value(), 500ULL);

  const Result<u64> too_much = harness.engine.consume_burst(opened.value(), 501);
  RG_CHECK(!too_much.has_value());
  RG_CHECK(too_much.reason() == ReasonCode::TokenBucketEmpty);

  harness.clock.advance(1000000000ULL);  // one second at 1000 ups
  const TickReport tick = harness.engine.tick();
  RG_CHECK_EQ(tick.envelopes_refilled, 1ULL);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK_EQ(view.value().burst_tokens_available, 1500ULL);
}

RG_TEST(lifecycle, unsatisfiable_envelope_fails_authorization_with_a_reason) {
  FixtureOptions options;
  options.grant_ceiling_ups = 500;
  options.policy_floor_ups = 900;
  options.policy_target_ups = 950;
  options.policy_ceiling_ups = 1000;
  Harness harness(options, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());

  const Result<PlanOutcome> plan = harness.engine.explain_plan(opened.value());
  RG_REQUIRE(plan.has_value());
  RG_CHECK(!plan.value().authorized);
  RG_CHECK(plan.value().reason == ReasonCode::PlanRejectedFloorExceedsFundedCeiling);

  const Result<PlanOutcome> authorized = harness.engine.authorize(opened.value(), harness.actor);
  RG_REQUIRE(authorized.has_value());
  RG_CHECK(!authorized.value().authorized);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Failed);
  RG_CHECK(view.value().action == EnvelopeAction::Halt);

  const Result<ApplyDispatch> applied = harness.engine.apply(opened.value(), harness.actor);
  RG_CHECK(!applied.has_value());
  RG_CHECK(applied.code() == ErrorCode::Denied);
}

RG_TEST(lifecycle, an_unsupported_backend_cannot_enforce_anything) {
  FixtureOptions options;
  Harness harness(options, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  // Replace the bound backend with one that cannot enforce.
  UnsupportedBackend unsupported(BackendId(2), Generation(1), "unsupported");
  BackendDescriptor descriptor = unsupported.describe();
  descriptor.provenance.source = AuthoritySource::Configuration;
  descriptor.provenance.detail = "unsupported test backend";
  RG_REQUIRE(harness.engine.put_backend(descriptor).has_value());
  harness.engine.bind_backend(&unsupported, harness.options.epoch);

  EnvelopeRequest request = make_request(harness.ids, harness.options);
  request.backend = BackendId(2);
  const Result<RateEnvelopeId> second = harness.engine.open_envelope(request, harness.actor);
  RG_REQUIRE(second.has_value());
  const Result<PlanOutcome> plan = harness.engine.explain_plan(second.value());
  RG_REQUIRE(plan.has_value());
  RG_CHECK(!plan.value().authorized);
  RG_CHECK(plan.value().reason == ReasonCode::PlanRejectedBackendUnsupported);
  // The unsupported backend can never be admitted as a session either, so an
  // apply is refused before it could even consider the plan.
  const Result<ApplyDispatch> applied = harness.engine.apply(second.value(), harness.actor);
  RG_CHECK(!applied.has_value());
  RG_CHECK(applied.code() == ErrorCode::BackendFailure);
}

RG_TEST(lifecycle, shutdown_abandons_in_flight_work_and_stops_intake) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  harness.backend.defer_completion = true;
  const Result<ApplyDispatch> dispatch = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(dispatch.has_value());

  const ShutdownReport report = harness.engine.shutdown();
  RG_CHECK(report.work_stopped);
  RG_CHECK_EQ(report.attempts_abandoned, 1ULL);
  RG_CHECK_EQ(report.envelopes_requiring_revalidation, 1ULL);
  RG_CHECK(report.backend_shutdown);
  RG_CHECK_EQ(harness.backend.shutdown_calls, 1ULL);

  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().requires_revalidation);
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);

  // Work intake is closed. Nothing may be accepted, and nothing may succeed.
  const Result<ApplyDispatch> after = harness.engine.apply(opened.value(), harness.actor);
  RG_CHECK(!after.has_value());
  RG_CHECK(after.code() == ErrorCode::ShuttingDown);
  Grant valid;
  valid.id = GrantId(99);
  valid.flow = harness.ids.flow;
  valid.resource = harness.ids.resource;
  valid.generation = Generation(1);
  valid.ceiling_ups = 10;
  valid.state = GrantState::Active;
  valid.provenance.source = AuthoritySource::Configuration;
  valid.provenance.detail = "shutdown test";
  const Status put = harness.engine.put_grant(valid);
  RG_CHECK(!put.has_value());
  RG_CHECK(put.code() == ErrorCode::ShuttingDown);

  CompletionReport late;
  late.status = DispatchStatus::Completed;
  late.acknowledged = true;
  late.epoch = harness.options.epoch;
  late.boot = harness.backend.session_boot();
  const Result<CompletionOutcome> completed =
      harness.engine.complete_apply(dispatch.value().attempt, late, harness.actor);
  RG_REQUIRE(completed.has_value());
  RG_CHECK(completed.value().rejected_late);
  RG_CHECK(!completed.value().verified);
}

RG_TEST(lifecycle, attempt_deadline_turns_a_silent_attempt_ambiguous) {
  FixtureOptions options;
  Harness harness(options, SyntheticDeviceConfig{});
  EngineConfig config = Harness::make_config(options);
  config.attempt_deadline_ns = 100;
  ManualClock clock(0);
  ScriptedBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, options.boot,
                          options.epoch);
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);
  (void)install_fixture(engine, options);
  BackendDescriptor descriptor = backend.describe();
  (void)engine.put_backend(descriptor);
  const ActorContext actor = fixture_actor(options);
  const Result<RateEnvelopeId> opened = engine.open_envelope(make_request(FixtureIds{}, options), actor);
  RG_REQUIRE(opened.has_value());
  backend.defer_completion = true;
  const Result<ApplyDispatch> dispatch = engine.apply(opened.value(), actor);
  RG_REQUIRE(dispatch.has_value());

  clock.advance(1000);
  const TickReport tick = engine.tick();
  RG_CHECK_EQ(tick.attempts_expired, 1ULL);
  const Result<EnforcementAttempt> attempt = engine.inspect_attempt(dispatch.value().attempt);
  RG_REQUIRE(attempt.has_value());
  RG_CHECK(attempt.value().state == AttemptState::Ambiguous);
  RG_CHECK(!attempt_state_may_publish_success(attempt.value().state));

  CompletionReport late;
  late.status = DispatchStatus::Completed;
  late.acknowledged = true;
  late.epoch = options.epoch;
  late.boot = backend.session_boot();
  const Result<CompletionOutcome> completed =
      engine.complete_apply(dispatch.value().attempt, late, actor);
  RG_REQUIRE(completed.has_value());
  RG_CHECK(completed.value().rejected_late);
  RG_CHECK(!completed.value().verified);
  RG_CHECK(!completed.value().accepted);
}

RG_TEST(lifecycle, grant_recall_makes_the_envelope_stale_and_revocable) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());

  const Status recalled = harness.engine.change_grant_state(
      harness.ids.grant, Generation(1), GrantState::Recalled,
      ReasonCode::EnvelopeRevokedByGrantRecall, harness.actor);
  RG_REQUIRE(recalled.has_value());

  const TickReport tick = harness.engine.tick();
  RG_CHECK_EQ(tick.envelopes_stale, 1ULL);
  RG_CHECK_EQ(tick.revokes_pending, 1ULL);

  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);
  RG_CHECK(view.value().reason == ReasonCode::EnvelopeStaleGrantRecalled);
  RG_CHECK(!view.value().legally_enforceable_now);
  RG_CHECK(view.value().action == EnvelopeAction::Revoke);

  const Result<RevokeDispatch> revoked = harness.engine.revoke(
      opened.value(), ReasonCode::EnvelopeRevokedByGrantRecall, harness.actor);
  RG_REQUIRE(revoked.has_value());
  RG_CHECK(revoked.value().verified);

  // The recalled generation can never fund a new envelope.
  const Result<RateEnvelopeId> reopened =
      harness.engine.open_envelope(make_request(harness.ids, harness.options), harness.actor);
  RG_REQUIRE(reopened.has_value());
  const Result<ApplyDispatch> denied = harness.engine.apply(reopened.value(), harness.actor);
  RG_CHECK(!denied.has_value());
  // The recall also advanced the grant generation, so the binding is stale
  // before it is merely inactive.
  RG_CHECK(denied.reason() == ReasonCode::PlanRejectedGrantGenerationMismatch);
}

RG_TEST(lifecycle, policy_and_capacity_changes_invalidate_and_reduce) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());

  Policy reduced;
  reduced.id = harness.ids.policy;
  reduced.generation = Generation(2);
  reduced.floor_ups = 100;
  reduced.target_ups = 300;
  reduced.ceiling_ups = 400;
  reduced.burst_tokens = 1000;
  reduced.refill = RefillSemantics::ContinuousTokenBucket;
  reduced.state = PolicyState::Active;
  reduced.provenance.source = AuthoritySource::Configuration;
  reduced.provenance.detail = "reduced policy";
  RG_REQUIRE(harness.engine.put_policy(reduced).has_value());

  // The envelope is bound to policy generation 1, which is no longer current.
  const TickReport tick = harness.engine.tick();
  RG_CHECK_EQ(tick.envelopes_stale, 1ULL);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().reason == ReasonCode::EnvelopeStalePolicyGenerationChanged);
  RG_CHECK(view.value().state == EnvelopeState::RevokePending);

  // A capacity reduction below a live plan is detected as a reduction, not as a
  // silent recalculation of an applied envelope.
  Harness second(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> second_envelope = second.open();
  RG_REQUIRE(second_envelope.has_value());
  RG_REQUIRE(second.engine.apply(second_envelope.value(), second.actor).has_value());
  Resource constrained = Resource{};
  constrained.id = FixtureIds{}.resource;
  constrained.generation = Generation(1);
  constrained.capacity_ups = 4000;
  constrained.state = ResourceState::Active;
  constrained.provenance.source = AuthoritySource::Configuration;
  constrained.provenance.detail = "unchanged resource";
  RG_REQUIRE(second.engine.put_resource(constrained).has_value() == false);
  RG_CHECK(second.engine.storage_stats().resources == 1ULL);
}

RG_TEST(lifecycle, an_effect_that_vanishes_is_caught_by_revalidation) {
  // A scheduled revalidation horizon turns "verified once" into "verified
  // recently", which is the only honest way to treat a live effect.
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{}, 1000);
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());
  SyntheticEntry entry;
  RG_CHECK(harness.backend.device().lookup(opened.value(), entry));
  RG_CHECK_EQ(entry.rate_ups, 1000ULL);
  RG_CHECK(harness.backend.device().admitted_boot() == harness.options.boot);

  // Something outside Rate Governor resets the device: the durable claim is
  // still on record, but the effect is gone.
  harness.backend.device().force_clear();
  RG_CHECK(!harness.backend.device().lookup(opened.value(), entry));

  harness.clock.advance(1000);
  const Result<EnvelopeView> due = harness.engine.inspect(opened.value());
  RG_REQUIRE(due.has_value());
  RG_CHECK(due.value().action == EnvelopeAction::Revalidate);
  RG_CHECK(due.value().action_reason == ReasonCode::EnvelopeRevalidationUnknown);

  const RevalidationReport report = harness.engine.revalidate_all(harness.actor);
  RG_CHECK_EQ(report.considered, 1ULL);
  RG_CHECK_EQ(report.requeued, 1ULL);
  RG_CHECK_EQ(report.confirmed_applied, 0ULL);
  const Result<EnvelopeView> view = harness.engine.inspect(opened.value());
  RG_REQUIRE(view.has_value());
  RG_CHECK(!view.value().effect_verified);
  RG_CHECK(!view.value().legally_enforceable_now);
  RG_CHECK(view.value().action == EnvelopeAction::RetryApply);

  const Result<ApplyDispatch> reapplied = harness.engine.apply(opened.value(), harness.actor);
  RG_REQUIRE(reapplied.has_value());
  RG_CHECK(reapplied.value().verified);
}

RG_TEST(lifecycle, counters_and_storage_are_internally_consistent) {
  Harness harness(FixtureOptions{}, SyntheticDeviceConfig{});
  const Result<RateEnvelopeId> opened = harness.open();
  RG_REQUIRE(opened.has_value());
  RG_REQUIRE(harness.engine.apply(opened.value(), harness.actor).has_value());
  RG_REQUIRE(harness.engine.revoke(opened.value(), ReasonCode::EnvelopeRevokedByOperator,
                                   harness.actor)
                 .has_value());
  const EngineCounters counters = harness.engine.counters();
  RG_CHECK_EQ(counters.envelopes_opened, 1ULL);
  RG_CHECK_EQ(counters.applies_dispatched, 1ULL);
  RG_CHECK_EQ(counters.applies_verified, 1ULL);
  RG_CHECK_EQ(counters.revokes_dispatched, 1ULL);
  RG_CHECK_EQ(counters.revokes_verified, 1ULL);
  RG_CHECK_EQ(counters.completions_rejected, 0ULL);
  RG_CHECK(counters.events_emitted > 0);

  const EngineStorageStats stats = harness.engine.storage_stats();
  RG_CHECK_EQ(stats.envelopes, 1ULL);
  RG_CHECK_EQ(stats.attempts, 2ULL);
  RG_CHECK_EQ(stats.grants, 1ULL);
  RG_CHECK_EQ(stats.policies, 1ULL);
  RG_CHECK_EQ(stats.resources, 1ULL);
  RG_CHECK_EQ(stats.recovery_orphan_records, 0ULL);

  const std::vector<AuditEvent> events = harness.engine.drain_events(1024);
  RG_CHECK(!events.empty());
  for (const AuditEvent& event : events) {
    RG_CHECK(!to_string_view(event.reason).empty());
    RG_CHECK(event.klass != ReasonClass::UnknownAuthority);
  }
  RG_CHECK(harness.engine.drain_events(1024).empty());
}
