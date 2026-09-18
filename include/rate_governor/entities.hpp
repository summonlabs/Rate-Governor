#ifndef RATE_GOVERNOR_ENTITIES_HPP
#define RATE_GOVERNOR_ENTITIES_HPP

// Authoritative control-plane entities.
//
// These are *inputs* to Rate Governor. Rate Governor does not decide who gets
// bandwidth: a grant, a reservation, a policy, a resource and a backend are all
// supplied by an external authority together with the generation that makes
// them current. Rate Governor's job is to decide what envelope those inputs
// legally support right now, to enforce it through a narrow backend
// abstraction, and to prove what actually happened.

#include <cstdint>
#include <string>
#include <string_view>

#include "rate_governor/clock.hpp"
#include "rate_governor/ids.hpp"
#include "rate_governor/provenance.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

enum class FlowState : std::uint8_t {
  Unknown = 0,
  Active,
  Withdrawn,
  Superseded,
};

enum class GrantState : std::uint8_t {
  Unknown = 0,
  Active,
  Recalled,
  Expired,
  Superseded,
};

enum class ReservationState : std::uint8_t {
  Unknown = 0,
  Active,
  Released,
  Expired,
  Superseded,
};

enum class PolicyState : std::uint8_t {
  Unknown = 0,
  Active,
  Suspended,
  Expired,
  Superseded,
};

enum class ResourceState : std::uint8_t {
  Unknown = 0,
  Active,
  Constrained,
  Withdrawn,
  Superseded,
};

enum class RefillSemantics : std::uint8_t {
  Unknown = 0,
  // Tokens accrue continuously with elapsed time, carrying an exact fractional
  // remainder so that split intervals and whole intervals agree exactly.
  ContinuousTokenBucket,
  // Tokens accrue only on explicit engine ticks; elapsed time between ticks is
  // accounted at the tick that observes it.
  DiscreteTickBucket,
};

[[nodiscard]] std::string_view to_string_view(FlowState state) noexcept;
[[nodiscard]] std::string_view to_string_view(GrantState state) noexcept;
[[nodiscard]] std::string_view to_string_view(ReservationState state) noexcept;
[[nodiscard]] std::string_view to_string_view(PolicyState state) noexcept;
[[nodiscard]] std::string_view to_string_view(ResourceState state) noexcept;
[[nodiscard]] std::string_view to_string_view(RefillSemantics semantics) noexcept;

// ---------------------------------------------------------------------------
// Flow: the governed traffic identity. Rate Governor does not decide what a
// flow contains; it only requires that the flow it authorizes is the flow that
// is current, at the generation the envelope was bound to.
// ---------------------------------------------------------------------------
struct Flow {
  FlowId id{};
  Generation generation{Generation(1)};
  FlowState state{FlowState::Unknown};
  Provenance provenance{};

  [[nodiscard]] bool active() const noexcept { return state == FlowState::Active; }
};

// ---------------------------------------------------------------------------
// Grant: funded ceiling authority for a flow on a resource.
// ---------------------------------------------------------------------------
struct Grant {
  GrantId id{};
  FlowId flow{};
  ResourceId resource{};
  Generation generation{Generation(1)};
  // Funded ceiling: the maximum rate the flow may be authorized to send on the
  // resource. Rate Governor may never authorize more than this.
  u64 ceiling_ups{0};
  // Burst allowance: extra tokens the flow may spend above the sustained rate.
  // A burst allowance is not permanent capacity and never raises the ceiling.
  u64 burst_tokens{0};
  ValidityWindow window{};
  GrantState state{GrantState::Unknown};
  Provenance provenance{};

  [[nodiscard]] bool active() const noexcept { return state == GrantState::Active; }
};

// ---------------------------------------------------------------------------
// Reservation: capacity set aside for a flow on a resource.
// ---------------------------------------------------------------------------
struct Reservation {
  ReservationId id{};
  FlowId flow{};
  ResourceId resource{};
  Generation generation{Generation(1)};
  u64 ceiling_ups{0};
  u64 burst_tokens{0};
  ValidityWindow window{};
  ReservationState state{ReservationState::Unknown};
  Provenance provenance{};

  [[nodiscard]] bool active() const noexcept { return state == ReservationState::Active; }
};

// ---------------------------------------------------------------------------
// Policy: the explicit rate envelope the operator wants {floor, target,
// ceiling} plus burst, refill, grace and hysteresis semantics.
// ---------------------------------------------------------------------------
struct HysteresisPolicy {
  // A verified applied rate at or below this level trips the reduction path
  // immediately instead of waiting for the next authorization.
  u64 reduce_below_ups{0};
  // A verified applied rate at or above this level is considered recovered.
  u64 recover_at_ups{0};
  // Minimum separation between two apply attempts for the same envelope.
  u64 cooldown_ns{0};
};

struct Policy {
  PolicyId id{};
  Generation generation{Generation(1)};
  u64 floor_ups{0};
  u64 target_ups{0};
  u64 ceiling_ups{0};
  u64 burst_tokens{0};
  RefillSemantics refill{RefillSemantics::Unknown};
  // Grace before a missed verification is escalated to a degradation.
  u64 grace_ns{0};
  HysteresisPolicy hysteresis{};
  ValidityWindow window{};
  PolicyState state{PolicyState::Unknown};
  Provenance provenance{};

  [[nodiscard]] bool active() const noexcept { return state == PolicyState::Active; }
  // floor <= target <= ceiling is a structural requirement of a policy; a
  // policy that violates it is malformed and is rejected at ingest.
  [[nodiscard]] bool ordering_holds() const noexcept {
    return floor_ups <= target_ups && target_ups <= ceiling_ups;
  }
};

// ---------------------------------------------------------------------------
// Resource: the governed object a flow is bound to, with its own capacity.
// ---------------------------------------------------------------------------
struct Resource {
  ResourceId id{};
  Generation generation{Generation(1)};
  // Hard capacity of the governed resource. Independent of any grant: this is
  // what the resource itself can carry.
  u64 capacity_ups{0};
  u64 capacity_burst_tokens{0};
  ResourceState state{ResourceState::Unknown};
  Provenance provenance{};

  [[nodiscard]] bool active() const noexcept {
    return state == ResourceState::Active || state == ResourceState::Constrained;
  }
};

// ---------------------------------------------------------------------------
// Backend: the narrow enforcement abstraction.
// ---------------------------------------------------------------------------
enum class BackendKind : std::uint8_t {
  Unknown = 0,
  // The backend cannot enforce a rate envelope at all. Rate Governor will
  // refuse to mark anything APPLIED through it.
  Unsupported,
  // The backend maintains an enforceable table and can read it back, but the
  // effect is a software simulation. It must never be described as physical
  // shaping, packet pacing or NIC offload.
  Synthetic,
  // Reserved for an embedder-supplied backend that programs real hardware.
  // Nothing in this repository claims or exercises this kind.
  Physical,
};

enum class VerificationMode : std::uint8_t {
  Unknown = 0,
  // No post-apply readback exists. Acknowledged effect can never become
  // APPLIED. The envelope stays DEGRADED with an explicit unsupported reason.
  None,
  // The backend can report the rate envelope it currently holds for a binding.
  PostApplyReadback,
};

inline constexpr std::uint32_t kBackendCapApply = 1U << 0;
inline constexpr std::uint32_t kBackendCapRevoke = 1U << 1;
inline constexpr std::uint32_t kBackendCapVerify = 1U << 2;
inline constexpr std::uint32_t kBackendCapBurst = 1U << 3;
inline constexpr std::uint32_t kBackendCapAsyncCompletion = 1U << 4;

struct BackendDescriptor {
  BackendId id{};
  Generation generation{Generation(1)};
  BackendKind kind{BackendKind::Unknown};
  std::uint32_t capabilities{0};
  VerificationMode verification{VerificationMode::Unknown};
  // Hard limits of the enforcement point, if it declares any. Zero means the
  // backend declares no limit, which is itself recorded as UNKNOWN capability
  // rather than as unlimited authority.
  u64 max_rate_ups{0};
  u64 max_burst_tokens{0};
  std::string label;
  Provenance provenance{};

  [[nodiscard]] bool can_apply() const noexcept {
    return (capabilities & kBackendCapApply) != 0;
  }
  [[nodiscard]] bool can_revoke() const noexcept {
    return (capabilities & kBackendCapRevoke) != 0;
  }
  [[nodiscard]] bool can_verify() const noexcept {
    return (capabilities & kBackendCapVerify) != 0 &&
           verification == VerificationMode::PostApplyReadback;
  }
  // Only a backend that can verify can ever support an APPLIED claim.
  [[nodiscard]] bool supports_verified_effect() const noexcept {
    return kind == BackendKind::Synthetic || kind == BackendKind::Physical;
  }
};

[[nodiscard]] std::string_view to_string_view(BackendKind kind) noexcept;
[[nodiscard]] std::string_view to_string_view(VerificationMode mode) noexcept;

// ---------------------------------------------------------------------------
// Structural validation. Returns ReasonCode::None when the entity is usable.
// These checks are about shape, not about authority: authority is decided at
// authorization time against the current generations and the current instant.
// ---------------------------------------------------------------------------
[[nodiscard]] ReasonCode validate_structure(const Flow& flow) noexcept;
[[nodiscard]] ReasonCode validate_structure(const Grant& grant) noexcept;
[[nodiscard]] ReasonCode validate_structure(const Reservation& reservation) noexcept;
[[nodiscard]] ReasonCode validate_structure(const Policy& policy) noexcept;
[[nodiscard]] ReasonCode validate_structure(const Resource& resource) noexcept;
[[nodiscard]] ReasonCode validate_structure(const BackendDescriptor& backend) noexcept;

[[nodiscard]] std::string describe(const Flow& flow);
[[nodiscard]] std::string describe(const Grant& grant);
[[nodiscard]] std::string describe(const Reservation& reservation);
[[nodiscard]] std::string describe(const Policy& policy);
[[nodiscard]] std::string describe(const Resource& resource);
[[nodiscard]] std::string describe(const BackendDescriptor& backend);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_ENTITIES_HPP
