#ifndef RATE_GOVERNOR_PLAN_HPP
#define RATE_GOVERNOR_PLAN_HPP

// Plan derivation: the single deterministic function that answers "what rate
// envelope is legally enforceable now, and why".
//
// The function is pure. It receives the exact authority snapshots the caller
// resolved, the instant of the decision, and nothing else. It cannot read a
// clock, a journal, a backend or a global. Given the same inputs it always
// returns the same plan and the same explanation, which is what makes an
// authoritative decision auditable after the fact.

#include <cstdint>
#include <string>

#include "rate_governor/backend.hpp"
#include "rate_governor/engine.hpp"
#include "rate_governor/entities.hpp"
#include "rate_governor/envelope.hpp"

namespace rate_governor {

// Null pointers mean "the named authority does not exist", which is itself a
// denial reason and never an implicit default.
struct PlanInputs {
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  FlowId flow{};
  Generation flow_generation{};
  const Flow* flow_def{nullptr};
  ResourceId resource{};
  Generation resource_generation{};
  const Resource* resource_def{nullptr};
  GrantId grant{};
  Generation grant_generation{};
  const Grant* grant_def{nullptr};
  bool has_reservation{false};
  ReservationId reservation{};
  Generation reservation_generation{};
  const Reservation* reservation_def{nullptr};
  PolicyId policy{};
  Generation policy_generation{};
  const Policy* policy_def{nullptr};
  BackendId backend{};
  Generation backend_generation{};
  const BackendDescriptor* backend_def{nullptr};
  FabricEpoch epoch{};
  WorkerBootId boot{};
};

// Derives the effective envelope. Never clamps upward: every magnitude in the
// result is bounded by the funding authority that supports it.
[[nodiscard]] PlanOutcome derive_plan(const PlanInputs& inputs, TimestampNs now);

// Re-derives the effective magnitudes from a fingerprint alone and reports
// whether they match the stored effective values. This is the check that a
// decision is still exactly the decision that was justified.
[[nodiscard]] bool fingerprint_self_consistent(const PlanFingerprint& fingerprint) noexcept;
[[nodiscard]] bool fingerprint_matches_authority(const PlanFingerprint& fingerprint,
                                                 const PlanInputs& inputs) noexcept;

[[nodiscard]] std::string explain_fingerprint(const PlanFingerprint& fingerprint);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_PLAN_HPP
