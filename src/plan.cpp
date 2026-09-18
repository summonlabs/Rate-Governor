#include "rate_governor/plan.hpp"

#include <string>

namespace rate_governor {
namespace {

struct Candidate {
  u64 value{0};
  LimitingAuthority authority{LimitingAuthority::Unknown};
};

void consider(Candidate& current, u64 value, LimitingAuthority authority) noexcept {
  if (value < current.value) {
    current.value = value;
    current.authority = authority;
  }
}

PlanOutcome deny(ReasonCode reason, std::string explanation) {
  PlanOutcome outcome;
  outcome.authorized = false;
  outcome.reason = reason;
  outcome.explanation = std::move(explanation);
  return outcome;
}

void append_magnitude(std::string& out, const char* label, u64 requested, u64 effective) {
  out.append(label);
  out.append(" requested=");
  out.append(std::to_string(requested));
  out.append(" effective=");
  out.append(std::to_string(effective));
}

}  // namespace

PlanOutcome derive_plan(const PlanInputs& inputs, TimestampNs now) {
  if (!inputs.envelope.valid() || !inputs.envelope_generation.valid()) {
    return deny(ReasonCode::PlanRejectedEnvelopeGenerationMismatch,
                "envelope identity or generation is not usable");
  }
  if (!inputs.flow.valid() || !inputs.resource.valid()) {
    return deny(ReasonCode::PlanRejectedUnknownFlow, "binding names an unusable flow or resource");
  }
  if (inputs.flow_def == nullptr) {
    return deny(ReasonCode::PlanRejectedUnknownFlow, "flow is not registered");
  }
  if (inputs.flow_def->generation != inputs.flow_generation) {
    return deny(ReasonCode::PlanRejectedFlowGenerationMismatch,
                "flow definition generation does not match the binding");
  }
  if (!inputs.flow_def->active()) {
    return deny(ReasonCode::PlanRejectedFlowNotActive, "flow is not active");
  }
  if (inputs.resource_def == nullptr) {
    return deny(ReasonCode::PlanRejectedUnknownResource, "resource is not registered");
  }
  if (inputs.resource_def->generation != inputs.resource_generation) {
    return deny(ReasonCode::PlanRejectedResourceGenerationMismatch,
                "resource definition generation does not match the binding");
  }
  if (!inputs.resource_def->active()) {
    return deny(ReasonCode::PlanRejectedResourceNotActive, "resource is not active");
  }
  if (inputs.resource_def->capacity_ups == 0) {
    return deny(ReasonCode::PlanRejectedResourceCapacityZero, "resource declares zero capacity");
  }
  if (inputs.grant_def == nullptr) {
    return deny(ReasonCode::PlanRejectedMissingGrant, "no funding grant is registered");
  }
  if (inputs.grant_def->generation != inputs.grant_generation) {
    return deny(ReasonCode::PlanRejectedGrantGenerationMismatch,
                "grant generation moved after the envelope was bound");
  }
  if (inputs.grant_def->flow != inputs.flow || inputs.grant_def->resource != inputs.resource) {
    return deny(ReasonCode::PlanRejectedUnknownFlow,
                "grant does not fund this flow on this resource");
  }
  if (!inputs.grant_def->active()) {
    return deny(ReasonCode::PlanRejectedGrantNotActive, "grant is not active");
  }
  if (now < inputs.grant_def->window.not_before_ns) {
    return deny(ReasonCode::PlanRejectedGrantNotYetValid, "grant is not yet valid");
  }
  if (!inputs.grant_def->window.contains(now)) {
    return deny(ReasonCode::PlanRejectedGrantExpired, "grant has expired");
  }
  if (inputs.policy_def == nullptr) {
    return deny(ReasonCode::PlanRejectedMissingPolicy, "no rate policy is registered");
  }
  if (inputs.policy_def->generation != inputs.policy_generation) {
    return deny(ReasonCode::PlanRejectedPolicyGenerationMismatch,
                "policy generation moved after the envelope was bound");
  }
  if (!inputs.policy_def->active()) {
    return deny(ReasonCode::PlanRejectedPolicyNotActive, "policy is not active");
  }
  if (now < inputs.policy_def->window.not_before_ns) {
    return deny(ReasonCode::PlanRejectedPolicyNotYetValid, "policy is not yet valid");
  }
  if (!inputs.policy_def->window.contains(now)) {
    return deny(ReasonCode::PlanRejectedPolicyExpired, "policy has expired");
  }
  if (!inputs.policy_def->ordering_holds()) {
    return deny(ReasonCode::MalformedInput, "policy violates floor <= target <= ceiling");
  }
  if (inputs.has_reservation) {
    if (inputs.reservation_def == nullptr) {
      return deny(ReasonCode::PlanRejectedMissingReservation,
                  "envelope requires a reservation that is not registered");
    }
    if (inputs.reservation_def->generation != inputs.reservation_generation) {
      return deny(ReasonCode::PlanRejectedReservationGenerationMismatch,
                  "reservation generation moved after the envelope was bound");
    }
    if (inputs.reservation_def->flow != inputs.flow ||
        inputs.reservation_def->resource != inputs.resource) {
      return deny(ReasonCode::PlanRejectedMissingReservation,
                  "reservation does not cover this flow on this resource");
    }
    if (!inputs.reservation_def->active()) {
      return deny(ReasonCode::PlanRejectedReservationNotActive, "reservation is not active");
    }
    if (now < inputs.reservation_def->window.not_before_ns) {
      return deny(ReasonCode::PlanRejectedReservationNotYetValid,
                  "reservation is not yet valid");
    }
    if (!inputs.reservation_def->window.contains(now)) {
      return deny(ReasonCode::PlanRejectedReservationExpired, "reservation has expired");
    }
  }
  if (inputs.backend_def == nullptr) {
    return deny(ReasonCode::PlanRejectedMissingBackend, "no enforcement backend is registered");
  }
  if (inputs.backend_def->generation != inputs.backend_generation) {
    return deny(ReasonCode::PlanRejectedBackendGenerationMismatch,
                "backend generation moved after the envelope was bound");
  }
  if (inputs.backend_def->kind == BackendKind::Unsupported || !inputs.backend_def->can_apply()) {
    return deny(ReasonCode::PlanRejectedBackendUnsupported,
                "backend cannot apply a rate envelope");
  }

  // Effective ceiling: never above any funding or capability authority.
  Candidate ceiling{inputs.grant_def->ceiling_ups, LimitingAuthority::Grant};
  if (inputs.has_reservation) {
    consider(ceiling, inputs.reservation_def->ceiling_ups, LimitingAuthority::Reservation);
  }
  consider(ceiling, inputs.policy_def->ceiling_ups, LimitingAuthority::Policy);
  consider(ceiling, inputs.resource_def->capacity_ups, LimitingAuthority::Resource);
  if (inputs.backend_def->max_rate_ups != 0) {
    consider(ceiling, inputs.backend_def->max_rate_ups, LimitingAuthority::Backend);
  }

  if (ceiling.value == 0) {
    return deny(ReasonCode::PlanRejectedCeilingZero, "funded ceiling is zero");
  }
  if (inputs.policy_def->floor_ups > ceiling.value) {
    return deny(ReasonCode::PlanRejectedFloorExceedsFundedCeiling,
                "policy floor exceeds the funded ceiling; the envelope is unsatisfiable");
  }

  const u64 target =
      clamp_u64(inputs.policy_def->target_ups, inputs.policy_def->floor_ups, ceiling.value);
  if (target < inputs.policy_def->floor_ups) {
    return deny(ReasonCode::PlanRejectedTargetBelowFloor, "target is below the policy floor");
  }

  Candidate burst{inputs.policy_def->burst_tokens, LimitingAuthority::Policy};
  consider(burst, inputs.grant_def->burst_tokens, LimitingAuthority::Grant);
  if (inputs.has_reservation) {
    consider(burst, inputs.reservation_def->burst_tokens, LimitingAuthority::Reservation);
  }
  if (inputs.resource_def->capacity_burst_tokens != 0) {
    consider(burst, inputs.resource_def->capacity_burst_tokens, LimitingAuthority::Resource);
  }
  if (inputs.backend_def->max_burst_tokens != 0) {
    consider(burst, inputs.backend_def->max_burst_tokens, LimitingAuthority::Backend);
  }

  TimestampNs valid_from = inputs.grant_def->window.not_before_ns;
  TimestampNs valid_until = inputs.grant_def->window.invalid_after_ns;
  if (valid_from < now) {
    valid_from = now;
  }
  if (inputs.policy_def->window.not_before_ns > valid_from) {
    valid_from = inputs.policy_def->window.not_before_ns;
  }
  if (inputs.policy_def->window.invalid_after_ns < valid_until) {
    valid_until = inputs.policy_def->window.invalid_after_ns;
  }
  if (inputs.has_reservation) {
    if (inputs.reservation_def->window.not_before_ns > valid_from) {
      valid_from = inputs.reservation_def->window.not_before_ns;
    }
    if (inputs.reservation_def->window.invalid_after_ns < valid_until) {
      valid_until = inputs.reservation_def->window.invalid_after_ns;
    }
  }

  PlanOutcome outcome;
  outcome.authorized = true;
  PlanFingerprint& fp = outcome.plan.fingerprint;
  fp.envelope = inputs.envelope;
  fp.envelope_generation = inputs.envelope_generation;
  fp.flow = inputs.flow;
  fp.flow_generation = inputs.flow_generation;
  fp.resource = inputs.resource;
  fp.resource_generation = inputs.resource_generation;
  fp.grant = inputs.grant;
  fp.grant_generation = inputs.grant_generation;
  fp.has_reservation = inputs.has_reservation;
  fp.reservation = inputs.reservation;
  fp.reservation_generation = inputs.reservation_generation;
  fp.policy = inputs.policy;
  fp.policy_generation = inputs.policy_generation;
  fp.backend = inputs.backend;
  fp.backend_generation = inputs.backend_generation;
  fp.epoch = inputs.epoch;
  fp.boot = inputs.boot;

  fp.granted_ceiling_ups = inputs.grant_def->ceiling_ups;
  fp.granted_burst_tokens = inputs.grant_def->burst_tokens;
  fp.reserved_ceiling_ups = inputs.has_reservation ? inputs.reservation_def->ceiling_ups : 0;
  fp.reserved_burst_tokens = inputs.has_reservation ? inputs.reservation_def->burst_tokens : 0;
  fp.policy_floor_ups = inputs.policy_def->floor_ups;
  fp.policy_target_ups = inputs.policy_def->target_ups;
  fp.policy_ceiling_ups = inputs.policy_def->ceiling_ups;
  fp.policy_burst_tokens = inputs.policy_def->burst_tokens;
  fp.resource_capacity_ups = inputs.resource_def->capacity_ups;
  fp.resource_capacity_burst_tokens = inputs.resource_def->capacity_burst_tokens;
  fp.backend_max_rate_ups = inputs.backend_def->max_rate_ups;
  fp.backend_max_burst_tokens = inputs.backend_def->max_burst_tokens;

  fp.effective_ceiling_ups = ceiling.value;
  fp.effective_floor_ups = inputs.policy_def->floor_ups;
  fp.effective_target_ups = target;
  fp.effective_burst_tokens = burst.value;
  fp.valid_from_ns = valid_from;
  fp.valid_until_ns = valid_until;

  outcome.plan.limiting_authority = ceiling.authority;
  outcome.plan.ceiling_clamped = ceiling.value < inputs.policy_def->ceiling_ups;
  outcome.plan.target_clamped = target != inputs.policy_def->target_ups;
  outcome.plan.burst_clamped = burst.value != inputs.policy_def->burst_tokens;
  outcome.plan.refill = inputs.policy_def->refill;
  outcome.plan.grace_ns = inputs.policy_def->grace_ns;
  outcome.plan.hysteresis = inputs.policy_def->hysteresis;

  if (!outcome.plan.invariants_hold()) {
    return deny(ReasonCode::MalformedInput, "derived plan violated a structural invariant");
  }
  if (!fingerprint_self_consistent(fp)) {
    return deny(ReasonCode::MalformedInput, "derived plan failed fingerprint consistency");
  }

  std::string explanation = "ceiling=";
  explanation.append(std::to_string(fp.effective_ceiling_ups));
  explanation.append(" ups limited_by=");
  explanation.append(to_string_view(ceiling.authority));
  if (outcome.plan.ceiling_clamped) {
    explanation.append(" (");
    append_magnitude(explanation, "ceiling", fp.policy_ceiling_ups, fp.effective_ceiling_ups);
    explanation.append(")");
  }
  explanation.append(" floor=");
  explanation.append(std::to_string(fp.effective_floor_ups));
  explanation.append(" target=");
  explanation.append(std::to_string(fp.effective_target_ups));
  if (outcome.plan.target_clamped) {
    explanation.append(" (");
    append_magnitude(explanation, "target", fp.policy_target_ups, fp.effective_target_ups);
    explanation.append(")");
  }
  explanation.append(" burst=");
  explanation.append(std::to_string(fp.effective_burst_tokens));
  if (outcome.plan.burst_clamped) {
    explanation.append(" (");
    append_magnitude(explanation, "burst", fp.policy_burst_tokens, fp.effective_burst_tokens);
    explanation.append(")");
  }
  explanation.append(" refill=");
  explanation.append(to_string_view(inputs.policy_def->refill));
  explanation.append(" window=[");
  explanation.append(std::to_string(fp.valid_from_ns));
  explanation.append(",");
  if (is_never(fp.valid_until_ns)) {
    explanation.append("never");
  } else {
    explanation.append(std::to_string(fp.valid_until_ns));
  }
  explanation.append(")");
  outcome.explanation = std::move(explanation);

  if (outcome.plan.target_clamped) {
    outcome.reason = ReasonCode::PlanTargetClampedByCeiling;
  } else if (outcome.plan.burst_clamped) {
    outcome.reason = ReasonCode::PlanBurstClampedByAuthority;
  } else {
    outcome.reason = ReasonCode::PlanAuthorized;
  }
  return outcome;
}

bool fingerprint_self_consistent(const PlanFingerprint& fingerprint) noexcept {
  Candidate ceiling{fingerprint.granted_ceiling_ups, LimitingAuthority::Grant};
  if (fingerprint.has_reservation) {
    consider(ceiling, fingerprint.reserved_ceiling_ups, LimitingAuthority::Reservation);
  }
  consider(ceiling, fingerprint.policy_ceiling_ups, LimitingAuthority::Policy);
  consider(ceiling, fingerprint.resource_capacity_ups, LimitingAuthority::Resource);
  if (fingerprint.backend_max_rate_ups != 0) {
    consider(ceiling, fingerprint.backend_max_rate_ups, LimitingAuthority::Backend);
  }
  if (fingerprint.effective_ceiling_ups != ceiling.value) {
    return false;
  }
  if (fingerprint.effective_floor_ups != fingerprint.policy_floor_ups) {
    return false;
  }
  if (fingerprint.effective_floor_ups > fingerprint.effective_ceiling_ups) {
    return false;
  }
  const u64 expected_target = clamp_u64(fingerprint.policy_target_ups,
                                        fingerprint.policy_floor_ups,
                                        fingerprint.effective_ceiling_ups);
  if (fingerprint.effective_target_ups != expected_target) {
    return false;
  }
  Candidate burst{fingerprint.policy_burst_tokens, LimitingAuthority::Policy};
  consider(burst, fingerprint.granted_burst_tokens, LimitingAuthority::Grant);
  if (fingerprint.has_reservation) {
    consider(burst, fingerprint.reserved_burst_tokens, LimitingAuthority::Reservation);
  }
  if (fingerprint.resource_capacity_burst_tokens != 0) {
    consider(burst, fingerprint.resource_capacity_burst_tokens, LimitingAuthority::Resource);
  }
  if (fingerprint.backend_max_burst_tokens != 0) {
    consider(burst, fingerprint.backend_max_burst_tokens, LimitingAuthority::Backend);
  }
  return fingerprint.effective_burst_tokens == burst.value;
}

bool fingerprint_matches_authority(const PlanFingerprint& fingerprint,
                                   const PlanInputs& inputs) noexcept {
  if (inputs.grant_def == nullptr || inputs.policy_def == nullptr ||
      inputs.resource_def == nullptr || inputs.backend_def == nullptr) {
    return false;
  }
  if (fingerprint.grant != inputs.grant ||
      fingerprint.grant_generation != inputs.grant_generation) {
    return false;
  }
  if (fingerprint.policy != inputs.policy ||
      fingerprint.policy_generation != inputs.policy_generation) {
    return false;
  }
  if (fingerprint.resource != inputs.resource ||
      fingerprint.resource_generation != inputs.resource_generation) {
    return false;
  }
  if (fingerprint.backend != inputs.backend ||
      fingerprint.backend_generation != inputs.backend_generation) {
    return false;
  }
  if (fingerprint.has_reservation != inputs.has_reservation) {
    return false;
  }
  if (inputs.has_reservation && (fingerprint.reservation != inputs.reservation ||
                                 fingerprint.reservation_generation !=
                                     inputs.reservation_generation)) {
    return false;
  }
  if (fingerprint.granted_ceiling_ups != inputs.grant_def->ceiling_ups ||
      fingerprint.granted_burst_tokens != inputs.grant_def->burst_tokens) {
    return false;
  }
  if (fingerprint.policy_ceiling_ups != inputs.policy_def->ceiling_ups ||
      fingerprint.policy_floor_ups != inputs.policy_def->floor_ups ||
      fingerprint.policy_target_ups != inputs.policy_def->target_ups ||
      fingerprint.policy_burst_tokens != inputs.policy_def->burst_tokens) {
    return false;
  }
  if (fingerprint.resource_capacity_ups != inputs.resource_def->capacity_ups ||
      fingerprint.resource_capacity_burst_tokens !=
          inputs.resource_def->capacity_burst_tokens) {
    return false;
  }
  if (fingerprint.backend_max_rate_ups != inputs.backend_def->max_rate_ups ||
      fingerprint.backend_max_burst_tokens != inputs.backend_def->max_burst_tokens) {
    return false;
  }
  return true;
}

std::string explain_fingerprint(const PlanFingerprint& fingerprint) {
  std::string out = "envelope=";
  out.append(fingerprint.envelope.to_string());
  out.append("/");
  out.append(fingerprint.envelope_generation.to_string());
  out.append(" flow=");
  out.append(fingerprint.flow.to_string());
  out.append("/");
  out.append(fingerprint.flow_generation.to_string());
  out.append(" resource=");
  out.append(fingerprint.resource.to_string());
  out.append("/");
  out.append(fingerprint.resource_generation.to_string());
  out.append(" grant=");
  out.append(fingerprint.grant.to_string());
  out.append("/");
  out.append(fingerprint.grant_generation.to_string());
  if (fingerprint.has_reservation) {
    out.append(" reservation=");
    out.append(fingerprint.reservation.to_string());
    out.append("/");
    out.append(fingerprint.reservation_generation.to_string());
  }
  out.append(" policy=");
  out.append(fingerprint.policy.to_string());
  out.append("/");
  out.append(fingerprint.policy_generation.to_string());
  out.append(" backend=");
  out.append(fingerprint.backend.to_string());
  out.append("/");
  out.append(fingerprint.backend_generation.to_string());
  out.append(" epoch=");
  out.append(fingerprint.epoch.to_string());
  out.append(" boot=");
  out.append(fingerprint.boot.to_string());
  return out;
}

}  // namespace rate_governor
