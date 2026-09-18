#ifndef RATE_GOVERNOR_TOOLS_SCENARIO_HPP
#define RATE_GOVERNOR_TOOLS_SCENARIO_HPP

// A deterministic offline scenario shared by rgctl and rg_bench. Everything it
// installs is synthetic control-plane state: one flow, one resource, one grant,
// one reservation, one policy and one in-process synthetic enforcement device.
// It is labelled synthetic wherever it is reported and it makes no claim about
// packets, links or hardware.

#include <string>

#include "rate_governor/rate_governor.hpp"

namespace rate_governor::tools {

struct ScenarioIds {
  FlowId flow{FlowId(1)};
  ResourceId resource{ResourceId(1)};
  GrantId grant{GrantId(1)};
  ReservationId reservation{ReservationId(1)};
  PolicyId policy{PolicyId(1)};
  BackendId backend{BackendId(1)};
  RateEnvelopeId envelope{};
};

struct ScenarioOptions {
  u64 ceiling_ups{1000};
  u64 target_ups{600};
  u64 floor_ups{100};
  u64 burst_tokens{2000};
  FabricEpoch epoch{FabricEpoch(7)};
  WorkerBootId boot{WorkerBootId(11)};
  bool with_reservation{true};
  bool reserve_below_grant{true};
};

inline Provenance scenario_provenance(std::string_view detail) {
  Provenance provenance;
  provenance.source = AuthoritySource::Configuration;
  provenance.detail.assign(detail.substr(0, kMaxProvenanceDetail));
  return provenance;
}

inline ScenarioIds install_scenario(RateGovernor& engine, const ScenarioOptions& options) {
  ScenarioIds ids;
  Flow flow;
  flow.id = ids.flow;
  flow.generation = Generation(1);
  flow.state = FlowState::Active;
  flow.provenance = scenario_provenance("scenario flow");
  (void)engine.put_flow(flow);

  Resource resource;
  resource.id = ids.resource;
  resource.generation = Generation(1);
  resource.capacity_ups = options.ceiling_ups * 4;
  resource.capacity_burst_tokens = options.burst_tokens * 4;
  resource.state = ResourceState::Active;
  resource.provenance = scenario_provenance("scenario resource");
  (void)engine.put_resource(resource);

  Grant grant;
  grant.id = ids.grant;
  grant.flow = ids.flow;
  grant.resource = ids.resource;
  grant.generation = Generation(1);
  grant.ceiling_ups = options.ceiling_ups;
  grant.burst_tokens = options.burst_tokens;
  grant.state = GrantState::Active;
  grant.provenance = scenario_provenance("scenario grant");
  (void)engine.put_grant(grant);

  if (options.with_reservation) {
    Reservation reservation;
    reservation.id = ids.reservation;
    reservation.flow = ids.flow;
    reservation.resource = ids.resource;
    reservation.generation = Generation(1);
    reservation.ceiling_ups =
        options.reserve_below_grant ? options.ceiling_ups - 1 : options.ceiling_ups;
    reservation.burst_tokens = options.burst_tokens;
    reservation.state = ReservationState::Active;
    reservation.provenance = scenario_provenance("scenario reservation");
    (void)engine.put_reservation(reservation);
  }

  Policy policy;
  policy.id = ids.policy;
  policy.generation = Generation(1);
  policy.floor_ups = options.floor_ups;
  policy.target_ups = options.target_ups;
  policy.ceiling_ups = options.ceiling_ups;
  policy.burst_tokens = options.burst_tokens;
  policy.refill = RefillSemantics::ContinuousTokenBucket;
  policy.state = PolicyState::Active;
  policy.provenance = scenario_provenance("scenario policy");
  policy.hysteresis.cooldown_ns = 1000;
  policy.hysteresis.reduce_below_ups = options.floor_ups;
  policy.hysteresis.recover_at_ups = options.target_ups;
  (void)engine.put_policy(policy);
  return ids;
}

inline ActorContext scenario_actor(const ScenarioOptions& options) {
  return operator_context(options.epoch, options.boot, "scenario actor");
}

}  // namespace rate_governor::tools

#endif  // RATE_GOVERNOR_TOOLS_SCENARIO_HPP
