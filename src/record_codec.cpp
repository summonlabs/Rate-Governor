#include "record_codec.hpp"

namespace rate_governor {
namespace {

void put_text(ByteWriter& writer, const std::string& text) {
  writer.text_unbounded(text.size() > kRecordTextHardLimit ? text.substr(0, kRecordTextHardLimit)
                                                           : text);
}

bool get_text(ByteReader& reader, std::string& out) {
  return reader.text_unbounded(kRecordTextHardLimit, out);
}

}  // namespace

void encode_provenance(ByteWriter& writer, const Provenance& provenance) {
  writer.u8(static_cast<u8>(provenance.source));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  writer.u64(provenance.epoch.value());
  writer.u64(provenance.boot.value());
  writer.u64(provenance.sequence);
  put_text(writer, provenance.detail);
}

bool decode_provenance(ByteReader& reader, Provenance& out) {
  u8 source = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u8(source) || !reader.u8(pad8) || !reader.u16(pad16) || !reader.u32(pad32) ||
      !reader.u64(epoch) || !reader.u64(boot) || !reader.u64(out.sequence)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.source = static_cast<AuthoritySource>(source);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return get_text(reader, out.detail);
}

void encode_flow(ByteWriter& writer, const Flow& flow) {
  writer.u64(flow.id.value());
  writer.u64(flow.generation.value());
  writer.u8(static_cast<u8>(flow.state));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  encode_provenance(writer, flow.provenance);
}

bool decode_flow(ByteReader& reader, Flow& out) {
  u64 id = 0;
  u64 generation = 0;
  u8 state = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !reader.u8(state) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.id = FlowId(id);
  out.generation = Generation(generation);
  out.state = static_cast<FlowState>(state);
  return decode_provenance(reader, out.provenance);
}

void encode_window(ByteWriter& writer, const ValidityWindow& window) {
  writer.u64(window.not_before_ns);
  writer.u64(window.invalid_after_ns);
}

bool decode_window(ByteReader& reader, ValidityWindow& out) {
  return reader.u64(out.not_before_ns) && reader.u64(out.invalid_after_ns);
}

void encode_grant(ByteWriter& writer, const Grant& grant) {
  writer.u64(grant.id.value());
  writer.u64(grant.flow.value());
  writer.u64(grant.resource.value());
  writer.u64(grant.generation.value());
  writer.u64(grant.ceiling_ups);
  writer.u64(grant.burst_tokens);
  encode_window(writer, grant.window);
  writer.u8(static_cast<u8>(grant.state));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  encode_provenance(writer, grant.provenance);
}

bool decode_grant(ByteReader& reader, Grant& out) {
  u64 id = 0;
  u64 flow = 0;
  u64 resource = 0;
  u64 generation = 0;
  u8 state = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(id) || !reader.u64(flow) || !reader.u64(resource) ||
      !reader.u64(generation) || !reader.u64(out.ceiling_ups) || !reader.u64(out.burst_tokens) ||
      !decode_window(reader, out.window) || !reader.u8(state) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.id = GrantId(id);
  out.flow = FlowId(flow);
  out.resource = ResourceId(resource);
  out.generation = Generation(generation);
  out.state = static_cast<GrantState>(state);
  return decode_provenance(reader, out.provenance);
}

void encode_reservation(ByteWriter& writer, const Reservation& reservation) {
  writer.u64(reservation.id.value());
  writer.u64(reservation.flow.value());
  writer.u64(reservation.resource.value());
  writer.u64(reservation.generation.value());
  writer.u64(reservation.ceiling_ups);
  writer.u64(reservation.burst_tokens);
  encode_window(writer, reservation.window);
  writer.u8(static_cast<u8>(reservation.state));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  encode_provenance(writer, reservation.provenance);
}

bool decode_reservation(ByteReader& reader, Reservation& out) {
  u64 id = 0;
  u64 flow = 0;
  u64 resource = 0;
  u64 generation = 0;
  u8 state = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(id) || !reader.u64(flow) || !reader.u64(resource) ||
      !reader.u64(generation) || !reader.u64(out.ceiling_ups) || !reader.u64(out.burst_tokens) ||
      !decode_window(reader, out.window) || !reader.u8(state) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.id = ReservationId(id);
  out.flow = FlowId(flow);
  out.resource = ResourceId(resource);
  out.generation = Generation(generation);
  out.state = static_cast<ReservationState>(state);
  return decode_provenance(reader, out.provenance);
}

void encode_policy(ByteWriter& writer, const Policy& policy) {
  writer.u64(policy.id.value());
  writer.u64(policy.generation.value());
  writer.u64(policy.floor_ups);
  writer.u64(policy.target_ups);
  writer.u64(policy.ceiling_ups);
  writer.u64(policy.burst_tokens);
  writer.u8(static_cast<u8>(policy.refill));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  writer.u64(policy.grace_ns);
  writer.u64(policy.hysteresis.reduce_below_ups);
  writer.u64(policy.hysteresis.recover_at_ups);
  writer.u64(policy.hysteresis.cooldown_ns);
  encode_window(writer, policy.window);
  writer.u8(static_cast<u8>(policy.state));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  encode_provenance(writer, policy.provenance);
}

bool decode_policy(ByteReader& reader, Policy& out) {
  u64 id = 0;
  u64 generation = 0;
  u8 refill = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  u8 state = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(out.floor_ups) ||
      !reader.u64(out.target_ups) || !reader.u64(out.ceiling_ups) ||
      !reader.u64(out.burst_tokens) || !reader.u8(refill) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32) || !reader.u64(out.grace_ns) ||
      !reader.u64(out.hysteresis.reduce_below_ups) || !reader.u64(out.hysteresis.recover_at_ups) ||
      !reader.u64(out.hysteresis.cooldown_ns) || !decode_window(reader, out.window) ||
      !reader.u8(state) || !reader.u8(pad8) || !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.id = PolicyId(id);
  out.generation = Generation(generation);
  out.refill = static_cast<RefillSemantics>(refill);
  out.state = static_cast<PolicyState>(state);
  return decode_provenance(reader, out.provenance);
}

void encode_resource(ByteWriter& writer, const Resource& resource) {
  writer.u64(resource.id.value());
  writer.u64(resource.generation.value());
  writer.u64(resource.capacity_ups);
  writer.u64(resource.capacity_burst_tokens);
  writer.u8(static_cast<u8>(resource.state));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  encode_provenance(writer, resource.provenance);
}

bool decode_resource(ByteReader& reader, Resource& out) {
  u64 id = 0;
  u64 generation = 0;
  u8 state = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(out.capacity_ups) ||
      !reader.u64(out.capacity_burst_tokens) || !reader.u8(state) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.id = ResourceId(id);
  out.generation = Generation(generation);
  out.state = static_cast<ResourceState>(state);
  return decode_provenance(reader, out.provenance);
}

void encode_backend(ByteWriter& writer, const BackendDescriptor& backend) {
  writer.u64(backend.id.value());
  writer.u64(backend.generation.value());
  writer.u8(static_cast<u8>(backend.kind));
  writer.u8(static_cast<u8>(backend.verification));
  writer.u16(0);
  writer.u32(backend.capabilities);
  writer.u64(backend.max_rate_ups);
  writer.u64(backend.max_burst_tokens);
  put_text(writer, backend.label);
  encode_provenance(writer, backend.provenance);
}

bool decode_backend(ByteReader& reader, BackendDescriptor& out) {
  u64 id = 0;
  u64 generation = 0;
  u8 kind = 0;
  u8 verification = 0;
  u16 pad16 = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !reader.u8(kind) ||
      !reader.u8(verification) || !reader.u16(pad16) || !reader.u32(out.capabilities) ||
      !reader.u64(out.max_rate_ups) || !reader.u64(out.max_burst_tokens)) {
    return false;
  }
  if (pad16 != 0) {
    return false;
  }
  out.id = BackendId(id);
  out.generation = Generation(generation);
  out.kind = static_cast<BackendKind>(kind);
  out.verification = static_cast<VerificationMode>(verification);
  return get_text(reader, out.label) && decode_provenance(reader, out.provenance);
}

void encode_fingerprint(ByteWriter& writer, const PlanFingerprint& fingerprint) {
  writer.u64(fingerprint.envelope.value());
  writer.u64(fingerprint.envelope_generation.value());
  writer.u64(fingerprint.flow.value());
  writer.u64(fingerprint.flow_generation.value());
  writer.u64(fingerprint.resource.value());
  writer.u64(fingerprint.resource_generation.value());
  writer.u64(fingerprint.grant.value());
  writer.u64(fingerprint.grant_generation.value());
  writer.u8(fingerprint.has_reservation ? 1U : 0U);
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  writer.u64(fingerprint.reservation.value());
  writer.u64(fingerprint.reservation_generation.value());
  writer.u64(fingerprint.policy.value());
  writer.u64(fingerprint.policy_generation.value());
  writer.u64(fingerprint.backend.value());
  writer.u64(fingerprint.backend_generation.value());
  writer.u64(fingerprint.epoch.value());
  writer.u64(fingerprint.boot.value());
  writer.u64(fingerprint.granted_ceiling_ups);
  writer.u64(fingerprint.granted_burst_tokens);
  writer.u64(fingerprint.reserved_ceiling_ups);
  writer.u64(fingerprint.reserved_burst_tokens);
  writer.u64(fingerprint.policy_floor_ups);
  writer.u64(fingerprint.policy_target_ups);
  writer.u64(fingerprint.policy_ceiling_ups);
  writer.u64(fingerprint.policy_burst_tokens);
  writer.u64(fingerprint.resource_capacity_ups);
  writer.u64(fingerprint.resource_capacity_burst_tokens);
  writer.u64(fingerprint.backend_max_rate_ups);
  writer.u64(fingerprint.backend_max_burst_tokens);
  writer.u64(fingerprint.effective_ceiling_ups);
  writer.u64(fingerprint.effective_floor_ups);
  writer.u64(fingerprint.effective_target_ups);
  writer.u64(fingerprint.effective_burst_tokens);
  writer.u64(fingerprint.valid_from_ns);
  writer.u64(fingerprint.valid_until_ns);
}

bool decode_fingerprint(ByteReader& reader, PlanFingerprint& out) {
  u64 envelope = 0;
  u64 envelope_generation = 0;
  u64 flow = 0;
  u64 flow_generation = 0;
  u64 resource = 0;
  u64 resource_generation = 0;
  u64 grant = 0;
  u64 grant_generation = 0;
  u8 has_reservation = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  u64 reservation = 0;
  u64 reservation_generation = 0;
  u64 policy = 0;
  u64 policy_generation = 0;
  u64 backend = 0;
  u64 backend_generation = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u64(envelope) || !reader.u64(envelope_generation) || !reader.u64(flow) ||
      !reader.u64(flow_generation) || !reader.u64(resource) || !reader.u64(resource_generation) ||
      !reader.u64(grant) || !reader.u64(grant_generation) || !reader.u8(has_reservation) ||
      !reader.u8(pad8) || !reader.u16(pad16) || !reader.u32(pad32) || !reader.u64(reservation) ||
      !reader.u64(reservation_generation) || !reader.u64(policy) || !reader.u64(policy_generation) ||
      !reader.u64(backend) || !reader.u64(backend_generation) || !reader.u64(epoch) ||
      !reader.u64(boot)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0 || has_reservation > 1U) {
    return false;
  }
  out.envelope = RateEnvelopeId(envelope);
  out.envelope_generation = Generation(envelope_generation);
  out.flow = FlowId(flow);
  out.flow_generation = Generation(flow_generation);
  out.resource = ResourceId(resource);
  out.resource_generation = Generation(resource_generation);
  out.grant = GrantId(grant);
  out.grant_generation = Generation(grant_generation);
  out.has_reservation = has_reservation == 1U;
  out.reservation = ReservationId(reservation);
  out.reservation_generation = Generation(reservation_generation);
  out.policy = PolicyId(policy);
  out.policy_generation = Generation(policy_generation);
  out.backend = BackendId(backend);
  out.backend_generation = Generation(backend_generation);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.u64(out.granted_ceiling_ups) && reader.u64(out.granted_burst_tokens) &&
         reader.u64(out.reserved_ceiling_ups) && reader.u64(out.reserved_burst_tokens) &&
         reader.u64(out.policy_floor_ups) && reader.u64(out.policy_target_ups) &&
         reader.u64(out.policy_ceiling_ups) && reader.u64(out.policy_burst_tokens) &&
         reader.u64(out.resource_capacity_ups) && reader.u64(out.resource_capacity_burst_tokens) &&
         reader.u64(out.backend_max_rate_ups) && reader.u64(out.backend_max_burst_tokens) &&
         reader.u64(out.effective_ceiling_ups) && reader.u64(out.effective_floor_ups) &&
         reader.u64(out.effective_target_ups) && reader.u64(out.effective_burst_tokens) &&
         reader.u64(out.valid_from_ns) && reader.u64(out.valid_until_ns);
}

void encode_plan(ByteWriter& writer, const EffectivePlan& plan) {
  encode_fingerprint(writer, plan.fingerprint);
  writer.u8(static_cast<u8>(plan.limiting_authority));
  writer.u8(plan.ceiling_clamped ? 1U : 0U);
  writer.u8(plan.target_clamped ? 1U : 0U);
  writer.u8(plan.burst_clamped ? 1U : 0U);
  writer.u8(static_cast<u8>(plan.refill));
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  writer.u64(plan.grace_ns);
  writer.u64(plan.hysteresis.reduce_below_ups);
  writer.u64(plan.hysteresis.recover_at_ups);
  writer.u64(plan.hysteresis.cooldown_ns);
  put_text(writer, plan.explanation);
}

bool decode_plan(ByteReader& reader, EffectivePlan& out) {
  if (!decode_fingerprint(reader, out.fingerprint)) {
    return false;
  }
  u8 limiting = 0;
  u8 ceiling_clamped = 0;
  u8 target_clamped = 0;
  u8 burst_clamped = 0;
  u8 refill = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u8(limiting) || !reader.u8(ceiling_clamped) || !reader.u8(target_clamped) ||
      !reader.u8(burst_clamped) || !reader.u8(refill) || !reader.u8(pad8) ||
      !reader.u16(pad16) || !reader.u32(pad32) || !reader.u64(out.grace_ns) ||
      !reader.u64(out.hysteresis.reduce_below_ups) || !reader.u64(out.hysteresis.recover_at_ups) ||
      !reader.u64(out.hysteresis.cooldown_ns)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0 || ceiling_clamped > 1U || target_clamped > 1U ||
      burst_clamped > 1U) {
    return false;
  }
  out.limiting_authority = static_cast<LimitingAuthority>(limiting);
  out.ceiling_clamped = ceiling_clamped == 1U;
  out.target_clamped = target_clamped == 1U;
  out.burst_clamped = burst_clamped == 1U;
  out.refill = static_cast<RefillSemantics>(refill);
  return get_text(reader, out.explanation);
}

void encode_bucket(ByteWriter& writer, const TokenBucket& bucket) {
  writer.u64(bucket.tokens);
  writer.u64(bucket.capacity);
  writer.u64(bucket.rate_ups);
  writer.u64(bucket.last_refill_ns);
  writer.u64(bucket.fractional_numerator);
  writer.u8(bucket.initialized ? 1U : 0U);
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
}

bool decode_bucket(ByteReader& reader, TokenBucket& out) {
  u8 initialized = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(out.tokens) || !reader.u64(out.capacity) || !reader.u64(out.rate_ups) ||
      !reader.u64(out.last_refill_ns) || !reader.u64(out.fractional_numerator) ||
      !reader.u8(initialized) || !reader.u8(pad8) || !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0 || initialized > 1U) {
    return false;
  }
  out.initialized = initialized == 1U;
  return true;
}

void encode_envelope(ByteWriter& writer, const RateEnvelope& envelope) {
  writer.u64(envelope.id.value());
  writer.u64(envelope.generation.value());
  writer.u64(envelope.flow.value());
  writer.u64(envelope.flow_generation.value());
  writer.u64(envelope.resource.value());
  writer.u64(envelope.resource_generation.value());
  writer.u64(envelope.grant.value());
  writer.u64(envelope.grant_generation.value());
  writer.u8(envelope.has_reservation ? 1U : 0U);
  writer.u8(0);
  writer.u16(0);
  writer.u32(0);
  writer.u64(envelope.reservation.value());
  writer.u64(envelope.reservation_generation.value());
  writer.u64(envelope.policy.value());
  writer.u64(envelope.policy_generation.value());
  writer.u64(envelope.backend.value());
  writer.u64(envelope.backend_generation.value());
  writer.u8(static_cast<u8>(envelope.state));
  writer.u8(0);
  writer.u16(static_cast<u16>(envelope.reason));
  writer.u32(0);
  writer.u8(envelope.has_plan ? 1U : 0U);
  writer.u8(envelope.effect_verified ? 1U : 0U);
  writer.u8(envelope.requires_revalidation ? 1U : 0U);
  writer.u8(0);
  if (envelope.has_plan) {
    encode_plan(writer, envelope.plan);
  }
  encode_bucket(writer, envelope.bucket);
  writer.u64(envelope.applied_rate_ups);
  writer.u64(envelope.applied_burst_tokens);
  writer.u64(envelope.applied_epoch.value());
  writer.u64(envelope.applied_boot.value());
  writer.u64(envelope.cooldown_until_ns);
  writer.u64(envelope.revalidate_by_ns);
  writer.u64(envelope.created_ns);
  writer.u64(envelope.updated_ns);
  writer.u64(envelope.plan_revision);
  writer.u64(envelope.attempts_issued);
  writer.u64(envelope.completions_applied);
  writer.u64(envelope.completions_rejected);
  put_text(writer, envelope.annotation);
}

bool decode_envelope(ByteReader& reader, RateEnvelope& out) {
  u64 id = 0;
  u64 generation = 0;
  u64 flow = 0;
  u64 flow_generation = 0;
  u64 resource = 0;
  u64 resource_generation = 0;
  u64 grant = 0;
  u64 grant_generation = 0;
  u8 has_reservation = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  u64 reservation = 0;
  u64 reservation_generation = 0;
  u64 policy = 0;
  u64 policy_generation = 0;
  u64 backend = 0;
  u64 backend_generation = 0;
  u8 state = 0;
  u16 reason = 0;
  u8 has_plan = 0;
  u8 effect_verified = 0;
  u8 requires_revalidation = 0;
  u8 pad8b = 0;
  u64 applied_epoch = 0;
  u64 applied_boot = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(flow) ||
      !reader.u64(flow_generation) || !reader.u64(resource) || !reader.u64(resource_generation) ||
      !reader.u64(grant) || !reader.u64(grant_generation) || !reader.u8(has_reservation) ||
      !reader.u8(pad8) || !reader.u16(pad16) || !reader.u32(pad32) || !reader.u64(reservation) ||
      !reader.u64(reservation_generation) || !reader.u64(policy) || !reader.u64(policy_generation) ||
      !reader.u64(backend) || !reader.u64(backend_generation) || !reader.u8(state) ||
      !reader.u8(pad8) || !reader.u16(reason) || !reader.u32(pad32) || !reader.u8(has_plan) ||
      !reader.u8(effect_verified) || !reader.u8(requires_revalidation) || !reader.u8(pad8b)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0 || pad32 != 0 || pad8b != 0 || has_reservation > 1U ||
      has_plan > 1U || effect_verified > 1U || requires_revalidation > 1U) {
    return false;
  }
  out.id = RateEnvelopeId(id);
  out.generation = Generation(generation);
  out.flow = FlowId(flow);
  out.flow_generation = Generation(flow_generation);
  out.resource = ResourceId(resource);
  out.resource_generation = Generation(resource_generation);
  out.grant = GrantId(grant);
  out.grant_generation = Generation(grant_generation);
  out.has_reservation = has_reservation == 1U;
  out.reservation = ReservationId(reservation);
  out.reservation_generation = Generation(reservation_generation);
  out.policy = PolicyId(policy);
  out.policy_generation = Generation(policy_generation);
  out.backend = BackendId(backend);
  out.backend_generation = Generation(backend_generation);
  out.state = static_cast<EnvelopeState>(state);
  out.reason = static_cast<ReasonCode>(reason);
  out.has_plan = has_plan == 1U;
  out.effect_verified = effect_verified == 1U;
  out.requires_revalidation = requires_revalidation == 1U;
  if (out.has_plan && !decode_plan(reader, out.plan)) {
    return false;
  }
  if (!decode_bucket(reader, out.bucket)) {
    return false;
  }
  if (!reader.u64(out.applied_rate_ups) || !reader.u64(out.applied_burst_tokens) ||
      !reader.u64(applied_epoch) || !reader.u64(applied_boot) ||
      !reader.u64(out.cooldown_until_ns) || !reader.u64(out.revalidate_by_ns) ||
      !reader.u64(out.created_ns) || !reader.u64(out.updated_ns) ||
      !reader.u64(out.plan_revision) || !reader.u64(out.attempts_issued) ||
      !reader.u64(out.completions_applied) || !reader.u64(out.completions_rejected)) {
    return false;
  }
  out.applied_epoch = FabricEpoch(applied_epoch);
  out.applied_boot = WorkerBootId(applied_boot);
  return get_text(reader, out.annotation);
}

void encode_attempt(ByteWriter& writer, const EnforcementAttempt& attempt) {
  writer.u64(attempt.id.value());
  writer.u64(attempt.envelope.value());
  writer.u64(attempt.envelope_generation.value());
  writer.u8(static_cast<u8>(attempt.kind));
  writer.u8(static_cast<u8>(attempt.state));
  writer.u16(static_cast<u16>(attempt.reason));
  writer.u32(0);
  encode_fingerprint(writer, attempt.fingerprint);
  writer.u64(attempt.backend.value());
  writer.u64(attempt.backend_generation.value());
  writer.u64(attempt.epoch.value());
  writer.u64(attempt.boot.value());
  writer.u64(attempt.sequence);
  writer.u64(attempt.idempotency_key);
  writer.u64(attempt.created_ns);
  writer.u64(attempt.dispatched_ns);
  writer.u64(attempt.deadline_ns);
  writer.u64(attempt.completed_ns);
  writer.u64(attempt.requested_rate_ups);
  writer.u64(attempt.requested_burst_tokens);
  writer.u64(attempt.acknowledged_rate_ups);
  writer.u64(attempt.acknowledged_burst_tokens);
  writer.u8(attempt.acknowledged ? 1U : 0U);
  writer.u8(attempt.verified ? 1U : 0U);
  writer.u8(attempt.effect_confirmed ? 1U : 0U);
  writer.u8(attempt.compensating ? 1U : 0U);
  writer.u64(attempt.verified_rate_ups);
  writer.u64(attempt.verified_burst_tokens);
  put_text(writer, attempt.detail);
}

bool decode_attempt(ByteReader& reader, EnforcementAttempt& out) {
  u64 id = 0;
  u64 envelope = 0;
  u64 envelope_generation = 0;
  u8 kind = 0;
  u8 state = 0;
  u16 reason = 0;
  u32 pad32 = 0;
  u64 backend = 0;
  u64 backend_generation = 0;
  u64 epoch = 0;
  u64 boot = 0;
  u8 acknowledged = 0;
  u8 verified = 0;
  u8 effect_confirmed = 0;
  u8 compensating = 0;
  if (!reader.u64(id) || !reader.u64(envelope) || !reader.u64(envelope_generation) ||
      !reader.u8(kind) || !reader.u8(state) || !reader.u16(reason) || !reader.u32(pad32)) {
    return false;
  }
  if (pad32 != 0) {
    return false;
  }
  out.id = EnforcementAttemptId(id);
  out.envelope = RateEnvelopeId(envelope);
  out.envelope_generation = Generation(envelope_generation);
  out.kind = static_cast<AttemptKind>(kind);
  out.state = static_cast<AttemptState>(state);
  out.reason = static_cast<ReasonCode>(reason);
  if (!decode_fingerprint(reader, out.fingerprint)) {
    return false;
  }
  if (!reader.u64(backend) || !reader.u64(backend_generation) || !reader.u64(epoch) ||
      !reader.u64(boot) || !reader.u64(out.sequence) || !reader.u64(out.idempotency_key) ||
      !reader.u64(out.created_ns) || !reader.u64(out.dispatched_ns) ||
      !reader.u64(out.deadline_ns) || !reader.u64(out.completed_ns) ||
      !reader.u64(out.requested_rate_ups) || !reader.u64(out.requested_burst_tokens) ||
      !reader.u64(out.acknowledged_rate_ups) || !reader.u64(out.acknowledged_burst_tokens) ||
      !reader.u8(acknowledged) || !reader.u8(verified) || !reader.u8(effect_confirmed) ||
      !reader.u8(compensating) || !reader.u64(out.verified_rate_ups) ||
      !reader.u64(out.verified_burst_tokens)) {
    return false;
  }
  if (acknowledged > 1U || verified > 1U || effect_confirmed > 1U || compensating > 1U) {
    return false;
  }
  out.backend = BackendId(backend);
  out.backend_generation = Generation(backend_generation);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  out.acknowledged = acknowledged == 1U;
  out.verified = verified == 1U;
  out.effect_confirmed = effect_confirmed == 1U;
  out.compensating = compensating == 1U;
  return get_text(reader, out.detail);
}

void encode_epoch_payload(ByteWriter& writer, FabricEpoch epoch, WorkerBootId boot,
                          ReasonCode reason) {
  writer.u64(epoch.value());
  writer.u64(boot.value());
  writer.u16(static_cast<u16>(reason));
  writer.u16(0);
  writer.u32(0);
}

bool decode_epoch_payload(ByteReader& reader, FabricEpoch& epoch, WorkerBootId& boot,
                          ReasonCode& reason) {
  u64 epoch_value = 0;
  u64 boot_value = 0;
  u16 reason_value = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(epoch_value) || !reader.u64(boot_value) || !reader.u16(reason_value) ||
      !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad16 != 0 || pad32 != 0) {
    return false;
  }
  epoch = FabricEpoch(epoch_value);
  boot = WorkerBootId(boot_value);
  reason = static_cast<ReasonCode>(reason_value);
  return true;
}

void encode_bucket_payload(ByteWriter& writer, const RateEnvelope& envelope) {
  writer.u64(envelope.id.value());
  writer.u64(envelope.generation.value());
  encode_bucket(writer, envelope.bucket);
  writer.u64(envelope.created_ns);
  writer.u64(envelope.updated_ns);
  writer.u64(envelope.attempts_issued);
  writer.u64(envelope.completions_applied);
  writer.u64(envelope.completions_rejected);
}

bool decode_bucket_payload(ByteReader& reader, RateEnvelope& out) {
  u64 id = 0;
  u64 generation = 0;
  if (!reader.u64(id) || !reader.u64(generation) || !decode_bucket(reader, out.bucket)) {
    return false;
  }
  if (!reader.u64(out.created_ns) || !reader.u64(out.updated_ns) ||
      !reader.u64(out.attempts_issued) || !reader.u64(out.completions_applied) ||
      !reader.u64(out.completions_rejected)) {
    return false;
  }
  out.id = RateEnvelopeId(id);
  out.generation = Generation(generation);
  return true;
}

void encode_grant_state_payload(ByteWriter& writer, GrantId id, Generation generation,
                                GrantState state, ReasonCode reason) {
  writer.u64(id.value());
  writer.u64(generation.value());
  writer.u8(static_cast<u8>(state));
  writer.u8(0);
  writer.u16(static_cast<u16>(reason));
  writer.u32(0);
}

bool decode_grant_state_payload(ByteReader& reader, GrantId& id, Generation& generation,
                                GrantState& state, ReasonCode& reason) {
  u64 id_value = 0;
  u64 generation_value = 0;
  u8 state_value = 0;
  u8 pad8 = 0;
  u16 reason_value = 0;
  u32 pad32 = 0;
  if (!reader.u64(id_value) || !reader.u64(generation_value) || !reader.u8(state_value) ||
      !reader.u8(pad8) || !reader.u16(reason_value) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0) {
    return false;
  }
  id = GrantId(id_value);
  generation = Generation(generation_value);
  state = static_cast<GrantState>(state_value);
  reason = static_cast<ReasonCode>(reason_value);
  return true;
}

void encode_reservation_state_payload(ByteWriter& writer, ReservationId id, Generation generation,
                                      ReservationState state, ReasonCode reason) {
  writer.u64(id.value());
  writer.u64(generation.value());
  writer.u8(static_cast<u8>(state));
  writer.u8(0);
  writer.u16(static_cast<u16>(reason));
  writer.u32(0);
}

bool decode_reservation_state_payload(ByteReader& reader, ReservationId& id, Generation& generation,
                                      ReservationState& state, ReasonCode& reason) {
  u64 id_value = 0;
  u64 generation_value = 0;
  u8 state_value = 0;
  u8 pad8 = 0;
  u16 reason_value = 0;
  u32 pad32 = 0;
  if (!reader.u64(id_value) || !reader.u64(generation_value) || !reader.u8(state_value) ||
      !reader.u8(pad8) || !reader.u16(reason_value) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0) {
    return false;
  }
  id = ReservationId(id_value);
  generation = Generation(generation_value);
  state = static_cast<ReservationState>(state_value);
  reason = static_cast<ReasonCode>(reason_value);
  return true;
}

void encode_policy_state_payload(ByteWriter& writer, PolicyId id, Generation generation,
                                 PolicyState state, ReasonCode reason) {
  writer.u64(id.value());
  writer.u64(generation.value());
  writer.u8(static_cast<u8>(state));
  writer.u8(0);
  writer.u16(static_cast<u16>(reason));
  writer.u32(0);
}

bool decode_policy_state_payload(ByteReader& reader, PolicyId& id, Generation& generation,
                                 PolicyState& state, ReasonCode& reason) {
  u64 id_value = 0;
  u64 generation_value = 0;
  u8 state_value = 0;
  u8 pad8 = 0;
  u16 reason_value = 0;
  u32 pad32 = 0;
  if (!reader.u64(id_value) || !reader.u64(generation_value) || !reader.u8(state_value) ||
      !reader.u8(pad8) || !reader.u16(reason_value) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0) {
    return false;
  }
  id = PolicyId(id_value);
  generation = Generation(generation_value);
  state = static_cast<PolicyState>(state_value);
  reason = static_cast<ReasonCode>(reason_value);
  return true;
}

void encode_resource_state_payload(ByteWriter& writer, ResourceId id, Generation generation,
                                   ResourceState state, ReasonCode reason) {
  writer.u64(id.value());
  writer.u64(generation.value());
  writer.u8(static_cast<u8>(state));
  writer.u8(0);
  writer.u16(static_cast<u16>(reason));
  writer.u32(0);
}

bool decode_resource_state_payload(ByteReader& reader, ResourceId& id, Generation& generation,
                                   ResourceState& state, ReasonCode& reason) {
  u64 id_value = 0;
  u64 generation_value = 0;
  u8 state_value = 0;
  u8 pad8 = 0;
  u16 reason_value = 0;
  u32 pad32 = 0;
  if (!reader.u64(id_value) || !reader.u64(generation_value) || !reader.u8(state_value) ||
      !reader.u8(pad8) || !reader.u16(reason_value) || !reader.u32(pad32)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0) {
    return false;
  }
  id = ResourceId(id_value);
  generation = Generation(generation_value);
  state = static_cast<ResourceState>(state_value);
  reason = static_cast<ReasonCode>(reason_value);
  return true;
}

}  // namespace rate_governor
