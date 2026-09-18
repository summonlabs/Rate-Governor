#include "rate_governor/entities.hpp"

#include <string>

#include "rate_governor/codec.hpp"
#include "rate_governor/envelope.hpp"

namespace rate_governor {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
IClock::~IClock() = default;

// ---------------------------------------------------------------------------
// Enum text
// ---------------------------------------------------------------------------
std::string_view to_string_view(FlowState state) noexcept {
  switch (state) {
    case FlowState::Unknown: return "Unknown";
    case FlowState::Active: return "Active";
    case FlowState::Withdrawn: return "Withdrawn";
    case FlowState::Superseded: return "Superseded";
  }
  return "Unknown";
}

std::string_view to_string_view(GrantState state) noexcept {
  switch (state) {
    case GrantState::Unknown: return "Unknown";
    case GrantState::Active: return "Active";
    case GrantState::Recalled: return "Recalled";
    case GrantState::Expired: return "Expired";
    case GrantState::Superseded: return "Superseded";
  }
  return "Unknown";
}

std::string_view to_string_view(ReservationState state) noexcept {
  switch (state) {
    case ReservationState::Unknown: return "Unknown";
    case ReservationState::Active: return "Active";
    case ReservationState::Released: return "Released";
    case ReservationState::Expired: return "Expired";
    case ReservationState::Superseded: return "Superseded";
  }
  return "Unknown";
}

std::string_view to_string_view(PolicyState state) noexcept {
  switch (state) {
    case PolicyState::Unknown: return "Unknown";
    case PolicyState::Active: return "Active";
    case PolicyState::Suspended: return "Suspended";
    case PolicyState::Expired: return "Expired";
    case PolicyState::Superseded: return "Superseded";
  }
  return "Unknown";
}

std::string_view to_string_view(ResourceState state) noexcept {
  switch (state) {
    case ResourceState::Unknown: return "Unknown";
    case ResourceState::Active: return "Active";
    case ResourceState::Constrained: return "Constrained";
    case ResourceState::Withdrawn: return "Withdrawn";
    case ResourceState::Superseded: return "Superseded";
  }
  return "Unknown";
}

std::string_view to_string_view(RefillSemantics semantics) noexcept {
  switch (semantics) {
    case RefillSemantics::Unknown: return "Unknown";
    case RefillSemantics::ContinuousTokenBucket: return "ContinuousTokenBucket";
    case RefillSemantics::DiscreteTickBucket: return "DiscreteTickBucket";
  }
  return "Unknown";
}

std::string_view to_string_view(BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::Unknown: return "Unknown";
    case BackendKind::Unsupported: return "Unsupported";
    case BackendKind::Synthetic: return "Synthetic";
    case BackendKind::Physical: return "Physical";
  }
  return "Unknown";
}

std::string_view to_string_view(VerificationMode mode) noexcept {
  switch (mode) {
    case VerificationMode::Unknown: return "Unknown";
    case VerificationMode::None: return "None";
    case VerificationMode::PostApplyReadback: return "PostApplyReadback";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------
std::string_view to_string_view(AuthoritySource source) noexcept {
  switch (source) {
    case AuthoritySource::Unknown: return "Unknown";
    case AuthoritySource::Operator: return "Operator";
    case AuthoritySource::Configuration: return "Configuration";
    case AuthoritySource::Recovery: return "Recovery";
    case AuthoritySource::BackendReport: return "BackendReport";
    case AuthoritySource::Derived: return "Derived";
  }
  return "Unknown";
}

bool is_bounded_text(std::string_view text, std::size_t max_length) noexcept {
  if (text.size() > max_length) {
    return false;
  }
  return is_ascii_printable(text);
}

std::string describe(const Provenance& provenance) {
  if (!provenance.authoritative()) {
    return std::string("provenance=UNKNOWN");
  }
  std::string out;
  out.append("provenance=");
  out.append(to_string_view(provenance.source));
  if (provenance.epoch.valid()) {
    out.append(" epoch=");
    out.append(provenance.epoch.to_string());
  }
  if (provenance.boot.valid()) {
    out.append(" boot=");
    out.append(provenance.boot.to_string());
  }
  if (provenance.sequence != 0) {
    out.append(" seq=");
    out.append(std::to_string(provenance.sequence));
  }
  if (!provenance.detail.empty()) {
    out.append(" detail=");
    out.append(provenance.detail);
  }
  return out;
}

ActorContext operator_context(FabricEpoch epoch, WorkerBootId boot, std::string_view detail) {
  ActorContext context;
  context.epoch = epoch;
  context.boot = boot;
  context.provenance.source = AuthoritySource::Operator;
  context.provenance.epoch = epoch;
  context.provenance.boot = boot;
  context.provenance.detail.assign(detail.substr(0, kMaxProvenanceDetail));
  return context;
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------
namespace {

bool provenance_ok(const Provenance& provenance) noexcept {
  return provenance.authoritative() && provenance.well_formed();
}

}  // namespace

ReasonCode validate_structure(const Flow& flow) noexcept {
  if (!flow.id.valid() || !flow.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (flow.state == FlowState::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (!provenance_ok(flow.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

ReasonCode validate_structure(const Grant& grant) noexcept {
  if (!grant.id.valid() || !grant.flow.valid() || !grant.resource.valid() ||
      !grant.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (grant.state == GrantState::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (grant.window.empty_window()) {
    return ReasonCode::MalformedInput;
  }
  if (!provenance_ok(grant.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

ReasonCode validate_structure(const Reservation& reservation) noexcept {
  if (!reservation.id.valid() || !reservation.flow.valid() || !reservation.resource.valid() ||
      !reservation.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (reservation.state == ReservationState::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (reservation.window.empty_window()) {
    return ReasonCode::MalformedInput;
  }
  if (!provenance_ok(reservation.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

ReasonCode validate_structure(const Policy& policy) noexcept {
  if (!policy.id.valid() || !policy.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (policy.state == PolicyState::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (policy.refill == RefillSemantics::Unknown) {
    return ReasonCode::MalformedInput;
  }
  if (!policy.ordering_holds()) {
    return ReasonCode::MalformedInput;
  }
  if (policy.window.empty_window()) {
    return ReasonCode::MalformedInput;
  }
  if (!provenance_ok(policy.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

ReasonCode validate_structure(const Resource& resource) noexcept {
  if (!resource.id.valid() || !resource.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (resource.state == ResourceState::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (!provenance_ok(resource.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

ReasonCode validate_structure(const BackendDescriptor& backend) noexcept {
  if (!backend.id.valid() || !backend.generation.valid()) {
    return ReasonCode::MalformedInput;
  }
  if (backend.kind == BackendKind::Unknown || backend.verification == VerificationMode::Unknown) {
    return ReasonCode::UnknownAuthority;
  }
  if (!is_bounded_text(backend.label, kMaxProvenanceDetail)) {
    return ReasonCode::MalformedInput;
  }
  if (!provenance_ok(backend.provenance)) {
    return ReasonCode::UnknownAuthority;
  }
  return ReasonCode::None;
}

// ---------------------------------------------------------------------------
// Descriptions
// ---------------------------------------------------------------------------
std::string describe(const Flow& flow) {
  std::string out = "flow ";
  out.append(flow.id.to_string());
  out.append(" gen=");
  out.append(flow.generation.to_string());
  out.append(" state=");
  out.append(to_string_view(flow.state));
  return out;
}

std::string describe(const Grant& grant) {
  std::string out = "grant ";
  out.append(grant.id.to_string());
  out.append(" flow=");
  out.append(grant.flow.to_string());
  out.append(" resource=");
  out.append(grant.resource.to_string());
  out.append(" gen=");
  out.append(grant.generation.to_string());
  out.append(" ceiling_ups=");
  out.append(std::to_string(grant.ceiling_ups));
  out.append(" burst_tokens=");
  out.append(std::to_string(grant.burst_tokens));
  out.append(" state=");
  out.append(to_string_view(grant.state));
  return out;
}

std::string describe(const Reservation& reservation) {
  std::string out = "reservation ";
  out.append(reservation.id.to_string());
  out.append(" flow=");
  out.append(reservation.flow.to_string());
  out.append(" ceiling_ups=");
  out.append(std::to_string(reservation.ceiling_ups));
  out.append(" burst_tokens=");
  out.append(std::to_string(reservation.burst_tokens));
  out.append(" state=");
  out.append(to_string_view(reservation.state));
  return out;
}

std::string describe(const Policy& policy) {
  std::string out = "policy ";
  out.append(policy.id.to_string());
  out.append(" gen=");
  out.append(policy.generation.to_string());
  out.append(" floor=");
  out.append(std::to_string(policy.floor_ups));
  out.append(" target=");
  out.append(std::to_string(policy.target_ups));
  out.append(" ceiling=");
  out.append(std::to_string(policy.ceiling_ups));
  out.append(" burst=");
  out.append(std::to_string(policy.burst_tokens));
  out.append(" refill=");
  out.append(to_string_view(policy.refill));
  out.append(" state=");
  out.append(to_string_view(policy.state));
  return out;
}

std::string describe(const Resource& resource) {
  std::string out = "resource ";
  out.append(resource.id.to_string());
  out.append(" gen=");
  out.append(resource.generation.to_string());
  out.append(" capacity_ups=");
  out.append(std::to_string(resource.capacity_ups));
  out.append(" state=");
  out.append(to_string_view(resource.state));
  return out;
}

std::string describe(const BackendDescriptor& backend) {
  std::string out = "backend ";
  out.append(backend.id.to_string());
  out.append(" gen=");
  out.append(backend.generation.to_string());
  out.append(" kind=");
  out.append(to_string_view(backend.kind));
  out.append(" verification=");
  out.append(to_string_view(backend.verification));
  out.append(" caps=0x");
  out.append(std::to_string(backend.capabilities));
  if (!backend.label.empty()) {
    out.append(" label=");
    out.append(backend.label);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Envelope helpers
// ---------------------------------------------------------------------------
std::string_view to_string_view(EnvelopeState state) noexcept {
  switch (state) {
    case EnvelopeState::Unknown: return "UNKNOWN";
    case EnvelopeState::Desired: return "DESIRED";
    case EnvelopeState::Authorized: return "AUTHORIZED";
    case EnvelopeState::Dispatching: return "DISPATCHING";
    case EnvelopeState::Applied: return "APPLIED";
    case EnvelopeState::Degraded: return "DEGRADED";
    case EnvelopeState::RevokePending: return "REVOKE_PENDING";
    case EnvelopeState::Revoked: return "REVOKED";
    case EnvelopeState::Stale: return "STALE";
    case EnvelopeState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

std::string_view to_string_view(EnvelopeAction action) noexcept {
  switch (action) {
    case EnvelopeAction::None: return "NONE";
    case EnvelopeAction::AwaitAuthorization: return "AWAIT_AUTHORIZATION";
    case EnvelopeAction::AwaitVerification: return "AWAIT_VERIFICATION";
    case EnvelopeAction::RetryApply: return "RETRY_APPLY";
    case EnvelopeAction::Reduce: return "REDUCE";
    case EnvelopeAction::Revoke: return "REVOKE";
    case EnvelopeAction::Revalidate: return "REVALIDATE";
    case EnvelopeAction::Fence: return "FENCE";
    case EnvelopeAction::RejectStale: return "REJECT_STALE";
    case EnvelopeAction::Halt: return "HALT";
  }
  return "NONE";
}

bool state_holds_enforceable_effect(EnvelopeState state) noexcept {
  return state == EnvelopeState::Applied || state == EnvelopeState::Degraded;
}

std::string_view to_string_view(LimitingAuthority authority) noexcept {
  switch (authority) {
    case LimitingAuthority::None: return "None";
    case LimitingAuthority::Grant: return "Grant";
    case LimitingAuthority::Reservation: return "Reservation";
    case LimitingAuthority::Policy: return "Policy";
    case LimitingAuthority::Resource: return "Resource";
    case LimitingAuthority::Backend: return "Backend";
    case LimitingAuthority::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::string_view to_string_view(AttemptKind kind) noexcept {
  switch (kind) {
    case AttemptKind::Unknown: return "Unknown";
    case AttemptKind::Apply: return "Apply";
    case AttemptKind::Revoke: return "Revoke";
  }
  return "Unknown";
}

std::string_view to_string_view(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Unknown: return "Unknown";
    case AttemptState::Created: return "Created";
    case AttemptState::Dispatched: return "Dispatched";
    case AttemptState::Acknowledged: return "Acknowledged";
    case AttemptState::Verified: return "Verified";
    case AttemptState::Failed: return "Failed";
    case AttemptState::Cancelled: return "Cancelled";
    case AttemptState::Abandoned: return "Abandoned";
    case AttemptState::LateRejected: return "LateRejected";
    case AttemptState::DuplicateIgnored: return "DuplicateIgnored";
    case AttemptState::Ambiguous: return "Ambiguous";
  }
  return "Unknown";
}

bool attempt_state_is_terminal(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Verified:
    case AttemptState::Failed:
    case AttemptState::Cancelled:
    case AttemptState::Abandoned:
    case AttemptState::LateRejected:
    case AttemptState::DuplicateIgnored:
    case AttemptState::Ambiguous:
      return true;
    case AttemptState::Unknown:
    case AttemptState::Created:
    case AttemptState::Dispatched:
    case AttemptState::Acknowledged:
      return false;
  }
  return false;
}

bool attempt_state_may_publish_success(AttemptState state) noexcept {
  // Only a verified attempt may publish success. An acknowledgement is
  // explicitly not enough, and an ambiguous, cancelled, abandoned or
  // late-rejected attempt may never publish success afterwards.
  return state == AttemptState::Verified;
}

u64 compute_idempotency_key(RateEnvelopeId envelope, Generation envelope_generation,
                           AttemptKind kind) noexcept {
  u64 state = mix64(envelope.value());
  state ^= mix64(envelope_generation.value() * 0x9E3779B97F4A7C15ULL);
  state ^= mix64(static_cast<u64>(kind) + 0x165667B19E3779F9ULL);
  return mix64(state);
}

bool PlanFingerprint::same_binding(const PlanFingerprint& other) const noexcept {
  return envelope == other.envelope && envelope_generation == other.envelope_generation &&
         flow == other.flow && flow_generation == other.flow_generation &&
         resource == other.resource && resource_generation == other.resource_generation &&
         grant == other.grant && grant_generation == other.grant_generation &&
         has_reservation == other.has_reservation && reservation == other.reservation &&
         reservation_generation == other.reservation_generation && policy == other.policy &&
         policy_generation == other.policy_generation && backend == other.backend &&
         backend_generation == other.backend_generation && epoch == other.epoch &&
         boot == other.boot;
}

bool EffectivePlan::invariants_hold() const noexcept {
  const PlanFingerprint& fp = fingerprint;
  if (fp.effective_ceiling_ups == 0) {
    return false;
  }
  if (fp.effective_floor_ups > fp.effective_target_ups) {
    return false;
  }
  if (fp.effective_target_ups > fp.effective_ceiling_ups) {
    return false;
  }
  if (!fp.envelope.valid() || !fp.envelope_generation.valid()) {
    return false;
  }
  if (!fp.grant.valid() || !fp.grant_generation.valid()) {
    return false;
  }
  if (!fp.policy.valid() || !fp.policy_generation.valid()) {
    return false;
  }
  if (!fp.resource.valid() || !fp.resource_generation.valid()) {
    return false;
  }
  if (!fp.backend.valid() || !fp.backend_generation.valid()) {
    return false;
  }
  if (!fp.epoch.valid() || !fp.boot.valid()) {
    return false;
  }
  return true;
}

}  // namespace rate_governor
