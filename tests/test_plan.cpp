#include "test_support.hpp"

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

Provenance plan_provenance(std::string_view detail) {
  Provenance provenance;
  provenance.source = AuthoritySource::Configuration;
  provenance.detail.assign(detail.substr(0, kMaxProvenanceDetail));
  return provenance;
}

struct Authority {
  Flow flow;
  Resource resource;
  Grant grant;
  Reservation reservation;
  Policy policy;
  BackendDescriptor backend;
};

Authority make_authority(u64 grant_ceiling = 1000, u64 grant_burst = 2000,
                         u64 policy_floor = 100, u64 policy_target = 600,
                         u64 policy_ceiling = 1000, u64 policy_burst = 2000,
                         u64 resource_capacity = 4000) {
  Authority authority;
  authority.flow.id = FlowId(1);
  authority.flow.generation = Generation(1);
  authority.flow.state = FlowState::Active;
  authority.flow.provenance = plan_provenance("plan flow");

  authority.resource.id = ResourceId(1);
  authority.resource.generation = Generation(1);
  authority.resource.capacity_ups = resource_capacity;
  authority.resource.state = ResourceState::Active;
  authority.resource.provenance = plan_provenance("plan resource");

  authority.grant.id = GrantId(1);
  authority.grant.flow = FlowId(1);
  authority.grant.resource = ResourceId(1);
  authority.grant.generation = Generation(1);
  authority.grant.ceiling_ups = grant_ceiling;
  authority.grant.burst_tokens = grant_burst;
  authority.grant.state = GrantState::Active;
  authority.grant.provenance = plan_provenance("plan grant");

  authority.reservation.id = ReservationId(1);
  authority.reservation.flow = FlowId(1);
  authority.reservation.resource = ResourceId(1);
  authority.reservation.generation = Generation(1);
  authority.reservation.ceiling_ups = grant_ceiling;
  authority.reservation.burst_tokens = grant_burst;
  authority.reservation.state = ReservationState::Active;
  authority.reservation.provenance = plan_provenance("plan reservation");

  authority.policy.id = PolicyId(1);
  authority.policy.generation = Generation(1);
  authority.policy.floor_ups = policy_floor;
  authority.policy.target_ups = policy_target;
  authority.policy.ceiling_ups = policy_ceiling;
  authority.policy.burst_tokens = policy_burst;
  authority.policy.refill = RefillSemantics::ContinuousTokenBucket;
  authority.policy.state = PolicyState::Active;
  authority.policy.provenance = plan_provenance("plan policy");

  authority.backend = fixture_backend_descriptor(BackendId(1), Generation(1));
  return authority;
}

PlanInputs make_inputs(const Authority& authority, bool with_reservation = false) {
  PlanInputs inputs;
  inputs.envelope = RateEnvelopeId(1);
  inputs.envelope_generation = Generation(1);
  inputs.flow = authority.flow.id;
  inputs.flow_generation = authority.flow.generation;
  inputs.flow_def = &authority.flow;
  inputs.resource = authority.resource.id;
  inputs.resource_generation = authority.resource.generation;
  inputs.resource_def = &authority.resource;
  inputs.grant = authority.grant.id;
  inputs.grant_generation = authority.grant.generation;
  inputs.grant_def = &authority.grant;
  inputs.has_reservation = with_reservation;
  inputs.reservation = authority.reservation.id;
  inputs.reservation_generation = authority.reservation.generation;
  inputs.reservation_def = with_reservation ? &authority.reservation : nullptr;
  inputs.policy = authority.policy.id;
  inputs.policy_generation = authority.policy.generation;
  inputs.policy_def = &authority.policy;
  inputs.backend = authority.backend.id;
  inputs.backend_generation = authority.backend.generation;
  inputs.backend_def = &authority.backend;
  inputs.epoch = FabricEpoch(1);
  inputs.boot = WorkerBootId(1);
  return inputs;
}

}  // namespace

RG_TEST(plan, effective_ceiling_is_the_minimum_of_every_authority) {
  {
    const Authority authority = make_authority(1000, 2000, 100, 600, 1000, 2000, 4000);
    const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 1000ULL);
    // Equal candidates resolve to the first authority considered: the grant.
    RG_CHECK(outcome.plan.limiting_authority == LimitingAuthority::Grant);
    RG_CHECK(outcome.plan.invariants_hold());
    RG_CHECK(fingerprint_self_consistent(outcome.plan.fingerprint));
  }
  {
    const Authority authority = make_authority(700, 2000, 100, 600, 1000, 2000, 4000);
    const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 700ULL);
    RG_CHECK(outcome.plan.limiting_authority == LimitingAuthority::Grant);
    RG_CHECK(outcome.plan.ceiling_clamped);
  }
  {
    const Authority authority = make_authority(1000, 2000, 100, 600, 1000, 2000, 500);
    const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 500ULL);
    RG_CHECK(outcome.plan.limiting_authority == LimitingAuthority::Resource);
  }
  {
    Authority authority = make_authority(1000, 2000, 100, 600, 1000, 2000, 4000);
    authority.backend.max_rate_ups = 400;
    const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 400ULL);
    RG_CHECK(outcome.plan.limiting_authority == LimitingAuthority::Backend);
  }
  {
    const Authority authority = make_authority(1000, 2000, 100, 600, 1000, 2000, 4000);
    const PlanOutcome outcome = derive_plan(make_inputs(authority, true), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 1000ULL);
  }
  {
    Authority authority = make_authority(1000, 2000, 100, 600, 1000, 2000, 4000);
    authority.reservation.ceiling_ups = 250;
    const PlanOutcome outcome = derive_plan(make_inputs(authority, true), 0);
    RG_REQUIRE(outcome.authorized);
    RG_CHECK_EQ(outcome.plan.ceiling_ups(), 250ULL);
    RG_CHECK(outcome.plan.limiting_authority == LimitingAuthority::Reservation);
  }
}

RG_TEST(plan, ceiling_never_exceeds_funding_authority_under_random_authority) {
  Rng rng(0xA11CEULL);
  for (int iteration = 0; iteration < 3000; ++iteration) {
    Authority authority = make_authority(1 + rng.below(5000), 1 + rng.below(5000), 0,
                                         1 + rng.below(5000), 1 + rng.below(5000),
                                         1 + rng.below(5000), 1 + rng.below(5000));
    authority.grant.ceiling_ups = 1 + rng.below(5000);
    authority.policy.floor_ups = rng.below(4000);
    authority.policy.target_ups = rng.below(6000);
    authority.policy.ceiling_ups = 1 + rng.below(6000);
    authority.backend.max_rate_ups = rng.chance(1, 2) ? 0 : 1 + rng.below(6000);
    const bool with_reservation = rng.chance(1, 2);
    const PlanOutcome outcome = derive_plan(make_inputs(authority, with_reservation), 0);

    u64 funded = authority.grant.ceiling_ups;
    if (with_reservation) {
      funded = std::min(funded, authority.reservation.ceiling_ups);
    }
    if (outcome.authorized) {
      RG_CHECK(outcome.plan.ceiling_ups() <= funded);
      RG_CHECK(outcome.plan.ceiling_ups() <= authority.policy.ceiling_ups);
      RG_CHECK(outcome.plan.ceiling_ups() <= authority.resource.capacity_ups);
      if (authority.backend.max_rate_ups != 0) {
        RG_CHECK(outcome.plan.ceiling_ups() <= authority.backend.max_rate_ups);
      }
      RG_CHECK(outcome.plan.floor_ups() <= outcome.plan.target_ups());
      RG_CHECK(outcome.plan.target_ups() <= outcome.plan.ceiling_ups());
      RG_CHECK(outcome.plan.invariants_hold());
      RG_CHECK(fingerprint_self_consistent(outcome.plan.fingerprint));
      RG_CHECK(fingerprint_matches_authority(outcome.plan.fingerprint, make_inputs(authority, with_reservation)));
      RG_CHECK(outcome.plan.ceiling_ups() > 0);
    } else {
      RG_CHECK(outcome.reason != ReasonCode::None);
      RG_CHECK(!outcome.plan.invariants_hold());
    }
  }
}

RG_TEST(plan, unsatisfiable_floor_is_denied_rather_than_silently_violated) {
  const Authority authority = make_authority(500, 2000, 900, 950, 1000, 2000, 4000);
  const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedFloorExceedsFundedCeiling);
  RG_CHECK(!outcome.explanation.empty());
}

RG_TEST(plan, target_is_clamped_into_the_funded_range) {
  const Authority authority = make_authority(500, 2000, 100, 900, 1000, 2000, 4000);
  const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
  RG_REQUIRE(outcome.authorized);
  RG_CHECK_EQ(outcome.plan.ceiling_ups(), 500ULL);
  RG_CHECK_EQ(outcome.plan.target_ups(), 500ULL);
  RG_CHECK(outcome.plan.target_clamped);
  RG_CHECK(outcome.reason == ReasonCode::PlanTargetClampedByCeiling);
  RG_CHECK(outcome.explanation.find("target") != std::string::npos);
}

RG_TEST(plan, burst_is_bounded_by_every_authority_that_declares_one) {
  Authority authority = make_authority(1000, 2000, 100, 600, 1000, 5000, 4000);
  authority.grant.burst_tokens = 800;
  const PlanOutcome outcome = derive_plan(make_inputs(authority), 0);
  RG_REQUIRE(outcome.authorized);
  RG_CHECK_EQ(outcome.plan.burst_tokens(), 800ULL);
  RG_CHECK(outcome.plan.burst_clamped);

  Authority zero_backend = make_authority(1000, 9000, 100, 600, 1000, 5000, 4000);
  zero_backend.backend.max_burst_tokens = 0;  // "not declared" is not "zero"
  const PlanOutcome outcome_two = derive_plan(make_inputs(zero_backend), 0);
  RG_REQUIRE(outcome_two.authorized);
  RG_CHECK_EQ(outcome_two.plan.burst_tokens(), 5000ULL);
}

RG_TEST(plan, stale_and_inactive_authority_is_denied) {
  // The envelope is bound to generation 1 while the authority has moved on.
  Authority bumped = make_authority();
  bumped.grant.generation = Generation(2);
  PlanInputs bumped_inputs = make_inputs(bumped);
  bumped_inputs.grant_generation = Generation(1);
  PlanOutcome outcome = derive_plan(bumped_inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedGrantGenerationMismatch);

  Authority bumped_policy = make_authority();
  bumped_policy.policy.generation = Generation(2);
  PlanInputs policy_inputs = make_inputs(bumped_policy);
  policy_inputs.policy_generation = Generation(1);
  outcome = derive_plan(policy_inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedPolicyGenerationMismatch);

  Authority bumped_resource = make_authority();
  bumped_resource.resource.generation = Generation(2);
  PlanInputs resource_inputs = make_inputs(bumped_resource);
  resource_inputs.resource_generation = Generation(1);
  outcome = derive_plan(resource_inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedResourceGenerationMismatch);

  Authority recalled = make_authority();
  recalled.grant.state = GrantState::Recalled;
  outcome = derive_plan(make_inputs(recalled), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedGrantNotActive);

  Authority suspended = make_authority();
  suspended.policy.state = PolicyState::Suspended;
  outcome = derive_plan(make_inputs(suspended), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedPolicyNotActive);

  Authority withdrawn = make_authority();
  withdrawn.resource.state = ResourceState::Withdrawn;
  outcome = derive_plan(make_inputs(withdrawn), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedResourceNotActive);

  Authority released = make_authority();
  released.reservation.state = ReservationState::Released;
  outcome = derive_plan(make_inputs(released, true), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedReservationNotActive);

  Authority missing_reservation = make_authority();
  PlanInputs inputs = make_inputs(missing_reservation, true);
  inputs.reservation_def = nullptr;
  outcome = derive_plan(inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedMissingReservation);
}

RG_TEST(plan, validity_windows_are_enforced_against_the_decision_instant) {
  Authority future = make_authority();
  future.grant.window.not_before_ns = 1000;
  PlanOutcome outcome = derive_plan(make_inputs(future), 999);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedGrantNotYetValid);
  outcome = derive_plan(make_inputs(future), 1000);
  RG_CHECK(outcome.authorized);

  Authority expired = make_authority();
  expired.grant.window.invalid_after_ns = 500;
  outcome = derive_plan(make_inputs(expired), 500);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedGrantExpired);

  Authority policy_expired = make_authority();
  policy_expired.policy.window.invalid_after_ns = 10;
  outcome = derive_plan(make_inputs(policy_expired), 10);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedPolicyExpired);

  Authority reservation_expired = make_authority();
  reservation_expired.reservation.window.invalid_after_ns = 10;
  outcome = derive_plan(make_inputs(reservation_expired, true), 10);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedReservationExpired);
}

RG_TEST(plan, zero_and_unsupported_authority_is_denied) {
  Authority zero_grant = make_authority(0, 2000, 0, 0, 1000, 2000, 4000);
  PlanOutcome outcome = derive_plan(make_inputs(zero_grant), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedCeilingZero);

  Authority zero_capacity = make_authority(1000, 2000, 100, 600, 1000, 2000, 0);
  outcome = derive_plan(make_inputs(zero_capacity), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedResourceCapacityZero);

  Authority unsupported = make_authority();
  unsupported.backend.kind = BackendKind::Unsupported;
  unsupported.backend.capabilities = 0;
  unsupported.backend.verification = VerificationMode::None;
  outcome = derive_plan(make_inputs(unsupported), 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedBackendUnsupported);

  Authority no_backend = make_authority();
  PlanInputs inputs = make_inputs(no_backend);
  inputs.backend_def = nullptr;
  outcome = derive_plan(inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedMissingBackend);

  Authority wrong_flow = make_authority();
  PlanInputs wrong_flow_inputs = make_inputs(wrong_flow);
  wrong_flow_inputs.flow = FlowId(2);
  outcome = derive_plan(wrong_flow_inputs, 0);
  RG_CHECK(!outcome.authorized);
  RG_CHECK(outcome.reason == ReasonCode::PlanRejectedUnknownFlow);
}

RG_TEST(plan, derivation_is_deterministic_and_explainable) {
  const Authority authority = make_authority(1000, 2000, 100, 600, 750, 2000, 4000);
  const PlanInputs inputs = make_inputs(authority);
  const PlanOutcome first = derive_plan(inputs, 12345);
  const PlanOutcome second = derive_plan(inputs, 12345);
  RG_REQUIRE(first.authorized && second.authorized);
  RG_CHECK_EQ(first.explanation, second.explanation);
  RG_CHECK(first.plan.fingerprint.same_binding(second.plan.fingerprint));
  RG_CHECK_EQ(first.plan.ceiling_ups(), second.plan.ceiling_ups());
  RG_CHECK_EQ(first.plan.burst_tokens(), second.plan.burst_tokens());
  RG_CHECK(first.plan.limiting_authority == second.plan.limiting_authority);
  RG_CHECK(!first.explanation.empty());
  RG_CHECK(first.explanation.find("limited_by=") != std::string::npos);
  RG_CHECK(!explain_fingerprint(first.plan.fingerprint).empty());

  // The window recorded is the intersection of the authorities' windows.
  Authority windowed = make_authority();
  windowed.grant.window.not_before_ns = 100;
  windowed.grant.window.invalid_after_ns = 900;
  windowed.policy.window.not_before_ns = 200;
  windowed.policy.window.invalid_after_ns = 800;
  const PlanOutcome windowed_outcome = derive_plan(make_inputs(windowed), 250);
  RG_REQUIRE(windowed_outcome.authorized);
  // The window starts at the later of the authorities' windows and "now".
  RG_CHECK_EQ(windowed_outcome.plan.fingerprint.valid_from_ns, 250ULL);
  RG_CHECK_EQ(windowed_outcome.plan.fingerprint.valid_until_ns, 800ULL);
  RG_CHECK(windowed_outcome.plan.fingerprint.valid_from_ns <=
           windowed_outcome.plan.fingerprint.valid_until_ns);
}

RG_TEST(plan, entity_structure_validation_rejects_malformed_input) {
  Authority authority = make_authority();
  RG_CHECK(validate_structure(authority.flow) == ReasonCode::None);
  RG_CHECK(validate_structure(authority.grant) == ReasonCode::None);
  RG_CHECK(validate_structure(authority.policy) == ReasonCode::None);
  RG_CHECK(validate_structure(authority.resource) == ReasonCode::None);
  RG_CHECK(validate_structure(authority.backend) == ReasonCode::None);

  Flow bad_flow = authority.flow;
  bad_flow.generation = Generation(0);
  RG_CHECK(validate_structure(bad_flow) == ReasonCode::MalformedInput);
  bad_flow = authority.flow;
  bad_flow.provenance.source = AuthoritySource::Unknown;
  RG_CHECK(validate_structure(bad_flow) == ReasonCode::UnknownAuthority);
  bad_flow = authority.flow;
  bad_flow.state = FlowState::Unknown;
  RG_CHECK(validate_structure(bad_flow) == ReasonCode::UnknownAuthority);

  Grant bad_grant = authority.grant;
  bad_grant.window.not_before_ns = 10;
  bad_grant.window.invalid_after_ns = 10;
  RG_CHECK(validate_structure(bad_grant) == ReasonCode::MalformedInput);
  bad_grant = authority.grant;
  bad_grant.id = GrantId(0);
  RG_CHECK(validate_structure(bad_grant) == ReasonCode::MalformedInput);

  Policy bad_policy = authority.policy;
  bad_policy.floor_ups = 700;
  bad_policy.target_ups = 600;
  RG_CHECK(validate_structure(bad_policy) == ReasonCode::MalformedInput);
  bad_policy = authority.policy;
  bad_policy.refill = RefillSemantics::Unknown;
  RG_CHECK(validate_structure(bad_policy) == ReasonCode::MalformedInput);

  BackendDescriptor bad_backend = authority.backend;
  bad_backend.kind = BackendKind::Unknown;
  RG_CHECK(validate_structure(bad_backend) == ReasonCode::UnknownAuthority);
  bad_backend = authority.backend;
  bad_backend.label = std::string(500, 'x');
  RG_CHECK(validate_structure(bad_backend) == ReasonCode::MalformedInput);
}

RG_TEST(plan, engine_ingest_refuses_generation_regression_and_respects_bounds) {
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);
  config.max_grants = 1;
  config.max_envelopes = 1;
  SyntheticDeviceConfig device;
  SyntheticBackend backend(BackendId(1), Generation(1), device, config.boot,
                           config.initial_epoch, "ingest test");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);
  const Authority authority = make_authority();

  RG_CHECK(engine.put_grant(authority.grant).has_value());
  // Same generation is not newer: refused, so an old controller cannot
  // resurrect superseded authority.
  const Status duplicate = engine.put_grant(authority.grant);
  RG_CHECK(!duplicate.has_value());
  RG_CHECK(duplicate.code() == ErrorCode::StaleAuthority);
  Grant newer = authority.grant;
  newer.generation = Generation(2);
  newer.ceiling_ups = 500;
  RG_CHECK(engine.put_grant(newer).has_value());
  Grant older = authority.grant;
  older.generation = Generation(1);
  RG_CHECK(!engine.put_grant(older).has_value());

  // Bounded tables.
  Grant extra = authority.grant;
  extra.id = GrantId(2);
  extra.generation = Generation(1);
  const Status overflow = engine.put_grant(extra);
  RG_CHECK(!overflow.has_value());
  RG_CHECK(overflow.code() == ErrorCode::LimitExceeded);
}
