#ifndef RATE_GOVERNOR_ENVELOPE_HPP
#define RATE_GOVERNOR_ENVELOPE_HPP

// The governed object: what envelope is legally enforceable right now, what
// effect has actually been applied, and what must happen next.

#include <cstdint>
#include <string>
#include <string_view>

#include "rate_governor/bucket.hpp"
#include "rate_governor/entities.hpp"
#include "rate_governor/ids.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

enum class EnvelopeState : std::uint8_t {
  // No durability, no evidence, no assumption.
  Unknown = 0,
  // Binding recorded, nothing authorized yet.
  Desired,
  // An effective plan exists that is legal under the current authorities.
  Authorized,
  // An apply attempt is in flight or awaiting verification.
  Dispatching,
  // The backend holds the envelope and the effect was read back and confirmed.
  Applied,
  // Something is enforced, but not what was authorized: under-delivery,
  // unverifiable effect, or recovered state awaiting revalidation.
  Degraded,
  // A revoke is required and not yet confirmed.
  RevokePending,
  // The backend confirmed no envelope is held.
  Revoked,
  // The authority that funded this envelope is gone or changed.
  Stale,
  // The envelope cannot exist as requested (unsatisfiable or malformed).
  Failed,
};

// What the operator must do next, derived only from durable state and the
// current instant. This is the answer to "when must the envelope be reduced,
// revoked, revalidated, fenced, or rejected as stale?".
enum class EnvelopeAction : std::uint8_t {
  None = 0,
  AwaitAuthorization,
  AwaitVerification,
  RetryApply,
  Reduce,
  Revoke,
  Revalidate,
  Fence,
  RejectStale,
  Halt,
};

[[nodiscard]] std::string_view to_string_view(EnvelopeState state) noexcept;
[[nodiscard]] std::string_view to_string_view(EnvelopeAction action) noexcept;
[[nodiscard]] bool state_holds_enforceable_effect(EnvelopeState state) noexcept;

// Which authority limited the effective ceiling. This is what makes the
// decision explainable rather than merely computed.
enum class LimitingAuthority : std::uint8_t {
  None = 0,
  Grant,
  Reservation,
  Policy,
  Resource,
  Backend,
  Unknown,
};

[[nodiscard]] std::string_view to_string_view(LimitingAuthority authority) noexcept;

// ---------------------------------------------------------------------------
// PlanFingerprint binds a decision to the exact evidence that justified it.
// Every generation is recorded, plus the raw magnitudes the decision used, so
// that a later re-derivation can be compared field by field.
// ---------------------------------------------------------------------------
struct PlanFingerprint {
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  FlowId flow{};
  Generation flow_generation{};
  ResourceId resource{};
  Generation resource_generation{};
  GrantId grant{};
  Generation grant_generation{};
  bool has_reservation{false};
  ReservationId reservation{};
  Generation reservation_generation{};
  PolicyId policy{};
  Generation policy_generation{};
  BackendId backend{};
  Generation backend_generation{};
  FabricEpoch epoch{};
  WorkerBootId boot{};

  u64 granted_ceiling_ups{0};
  u64 granted_burst_tokens{0};
  u64 reserved_ceiling_ups{0};
  u64 reserved_burst_tokens{0};
  u64 policy_floor_ups{0};
  u64 policy_target_ups{0};
  u64 policy_ceiling_ups{0};
  u64 policy_burst_tokens{0};
  u64 resource_capacity_ups{0};
  u64 resource_capacity_burst_tokens{0};
  // Declared backend limits, carried so that a re-derivation can reproduce the
  // exact ceiling the original decision used. Zero means "not declared".
  u64 backend_max_rate_ups{0};
  u64 backend_max_burst_tokens{0};

  u64 effective_ceiling_ups{0};
  u64 effective_floor_ups{0};
  u64 effective_target_ups{0};
  u64 effective_burst_tokens{0};

  TimestampNs valid_from_ns{0};
  TimestampNs valid_until_ns{kNeverExpiresNs};

  [[nodiscard]] bool same_binding(const PlanFingerprint& other) const noexcept;
};

// The effective envelope that is legally enforceable right now.
struct EffectivePlan {
  PlanFingerprint fingerprint{};
  LimitingAuthority limiting_authority{LimitingAuthority::None};
  bool ceiling_clamped{false};
  bool target_clamped{false};
  bool burst_clamped{false};
  RefillSemantics refill{RefillSemantics::Unknown};
  u64 grace_ns{0};
  HysteresisPolicy hysteresis{};
  std::string explanation;

  [[nodiscard]] u64 ceiling_ups() const noexcept { return fingerprint.effective_ceiling_ups; }
  [[nodiscard]] u64 floor_ups() const noexcept { return fingerprint.effective_floor_ups; }
  [[nodiscard]] u64 target_ups() const noexcept { return fingerprint.effective_target_ups; }
  [[nodiscard]] u64 burst_tokens() const noexcept { return fingerprint.effective_burst_tokens; }

  // Structural invariants that must hold for any plan the engine publishes.
  [[nodiscard]] bool invariants_hold() const noexcept;
};

// ---------------------------------------------------------------------------
// Requested binding. A generation of zero means "resolve to whatever is
// current when this envelope is authorized"; the resolved generation is always
// recorded in the fingerprint, never left implicit.
// ---------------------------------------------------------------------------
struct EnvelopeRequest {
  FlowId flow{};
  Generation flow_generation{};
  ResourceId resource{};
  Generation resource_generation{};
  GrantId grant{};
  Generation grant_generation{};
  bool has_reservation{false};
  ReservationId reservation{};
  Generation reservation_generation{};
  PolicyId policy{};
  Generation policy_generation{};
  BackendId backend{};
  Generation backend_generation{};
  std::string annotation;
};

// ---------------------------------------------------------------------------
// Enforcement attempts
// ---------------------------------------------------------------------------
enum class AttemptKind : std::uint8_t { Unknown = 0, Apply, Revoke };

enum class AttemptState : std::uint8_t {
  Unknown = 0,
  Created,
  Dispatched,
  Acknowledged,
  Verified,
  Failed,
  Cancelled,
  // The worker that owed this attempt died or lost its session.
  Abandoned,
  // Completion arrived but the attempt's authority had already expired.
  LateRejected,
  // Completion arrived twice; the second arrival changed nothing.
  DuplicateIgnored,
  // The outcome is unknown and must be revalidated before it can be trusted.
  Ambiguous,
};

[[nodiscard]] std::string_view to_string_view(AttemptKind kind) noexcept;
[[nodiscard]] std::string_view to_string_view(AttemptState state) noexcept;
[[nodiscard]] bool attempt_state_is_terminal(AttemptState state) noexcept;
// Only a verified attempt may publish success.
[[nodiscard]] bool attempt_state_may_publish_success(AttemptState state) noexcept;

struct EnforcementAttempt {
  EnforcementAttemptId id{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  AttemptKind kind{AttemptKind::Unknown};
  AttemptState state{AttemptState::Unknown};
  ReasonCode reason{ReasonCode::None};
  PlanFingerprint fingerprint{};
  BackendId backend{};
  Generation backend_generation{};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  u64 sequence{0};
  // Idempotency key: mix of (envelope, envelope generation, attempt kind).
  u64 idempotency_key{0};
  TimestampNs created_ns{0};
  TimestampNs dispatched_ns{0};
  TimestampNs deadline_ns{0};
  TimestampNs completed_ns{0};
  u64 requested_rate_ups{0};
  u64 requested_burst_tokens{0};
  u64 acknowledged_rate_ups{0};
  u64 acknowledged_burst_tokens{0};
  bool acknowledged{false};
  u64 verified_rate_ups{0};
  u64 verified_burst_tokens{0};
  bool verified{false};
  bool effect_confirmed{false};
  bool compensating{false};
  std::string detail;

  [[nodiscard]] bool inflight() const noexcept {
    return state == AttemptState::Created || state == AttemptState::Dispatched ||
           state == AttemptState::Acknowledged;
  }
};

[[nodiscard]] u64 compute_idempotency_key(RateEnvelopeId envelope, Generation envelope_generation,
                                          AttemptKind kind) noexcept;

// ---------------------------------------------------------------------------
// The governed envelope
// ---------------------------------------------------------------------------
struct RateEnvelope {
  RateEnvelopeId id{};
  Generation generation{Generation(1)};
  FlowId flow{};
  Generation flow_generation{};
  ResourceId resource{};
  Generation resource_generation{};
  GrantId grant{};
  Generation grant_generation{};
  bool has_reservation{false};
  ReservationId reservation{};
  Generation reservation_generation{};
  PolicyId policy{};
  Generation policy_generation{};
  BackendId backend{};
  Generation backend_generation{};

  EnvelopeState state{EnvelopeState::Unknown};
  ReasonCode reason{ReasonCode::None};
  bool has_plan{false};
  EffectivePlan plan{};
  TokenBucket bucket{};

  // Effect that the backend actually holds, as confirmed by readback.
  u64 applied_rate_ups{0};
  u64 applied_burst_tokens{0};
  bool effect_verified{false};
  FabricEpoch applied_epoch{};
  WorkerBootId applied_boot{};

  TimestampNs cooldown_until_ns{0};
  TimestampNs revalidate_by_ns{0};
  bool requires_revalidation{false};
  std::string annotation;
  TimestampNs created_ns{0};
  TimestampNs updated_ns{0};
  u64 plan_revision{0};
  u64 attempts_issued{0};
  u64 completions_applied{0};
  u64 completions_rejected{0};
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_ENVELOPE_HPP
