#ifndef RATE_GOVERNOR_REASON_HPP
#define RATE_GOVERNOR_REASON_HPP

// Bounded, exhaustive reason vocabulary.
//
// Every authoritative transition in Rate Governor names a reason from this
// closed set. Free-text reasons are never persisted as authority; they are
// annotations on top of a ReasonCode.

#include <cstdint>
#include <string_view>

namespace rate_governor {

enum class ReasonCode : std::uint16_t {
  None = 0,

  // Authorization / planning
  PlanAuthorized = 1,
  PlanTargetClampedByCeiling = 2,
  PlanBurstClampedByAuthority = 3,
  PlanRejectedFloorExceedsFundedCeiling = 4,
  PlanRejectedTargetBelowFloor = 5,
  PlanRejectedUnknownFlow = 6,
  PlanRejectedUnknownResource = 7,
  PlanRejectedMissingGrant = 8,
  PlanRejectedMissingPolicy = 9,
  PlanRejectedMissingReservation = 10,
  PlanRejectedMissingBackend = 11,
  PlanRejectedFlowGenerationMismatch = 12,
  PlanRejectedGrantGenerationMismatch = 13,
  PlanRejectedReservationGenerationMismatch = 14,
  PlanRejectedPolicyGenerationMismatch = 15,
  PlanRejectedResourceGenerationMismatch = 16,
  PlanRejectedBackendGenerationMismatch = 17,
  PlanRejectedGrantNotActive = 18,
  PlanRejectedPolicyNotActive = 19,
  PlanRejectedResourceNotActive = 20,
  PlanRejectedReservationNotActive = 21,
  PlanRejectedGrantNotYetValid = 22,
  PlanRejectedGrantExpired = 23,
  PlanRejectedReservationNotYetValid = 24,
  PlanRejectedReservationExpired = 25,
  PlanRejectedPolicyExpired = 26,
  PlanRejectedPolicyNotYetValid = 27,
  PlanRejectedBackendUnsupported = 28,
  PlanRejectedEnvelopeGenerationMismatch = 29,
  PlanRejectedResourceCapacityZero = 30,
  PlanRejectedCeilingZero = 31,
  PlanRejectedFlowNotActive = 32,

  // Enforcement attempts
  ApplyDispatched = 40,
  ApplyAcknowledged = 41,
  ApplyVerified = 42,
  ApplyUnderDelivered = 43,
  ApplyOverDeliveredRevoking = 44,
  ApplyBackendFailed = 45,
  ApplyVerificationUnsupported = 46,
  ApplyVerificationMismatch = 47,
  ApplyVerificationUnknown = 48,
  RevokeDispatched = 49,
  RevokeVerified = 50,
  RevokeBackendFailed = 51,
  AttemptCancelled = 52,
  AttemptLateCompletionRejected = 53,
  AttemptDuplicateIgnored = 54,
  AttemptAbandonedWorkerDeath = 55,
  AttemptDeadlineExceeded = 56,
  AttemptFencedByEpoch = 57,
  AttemptFencedByBoot = 58,
  AttemptFencedByGeneration = 59,
  AttemptSupersededByNewerAttempt = 60,
  AttemptIdempotentReplay = 61,
  CompensatingRevokeIssued = 62,

  // Envelope lifecycle
  EnvelopeOpened = 70,
  EnvelopeAuthorized = 71,
  EnvelopeRevokePending = 72,
  EnvelopeRevoked = 73,
  EnvelopeDegraded = 74,
  EnvelopeFailed = 75,
  EnvelopeStaleGrantRecalled = 76,
  EnvelopeStaleGrantExpired = 77,
  EnvelopeStaleReservationExpired = 78,
  EnvelopeStalePolicyExpired = 79,
  EnvelopeStalePolicyGenerationChanged = 80,
  EnvelopeStaleResourceGenerationChanged = 81,
  EnvelopeStaleResourceCapacityReduced = 82,
  EnvelopeStaleFlowGenerationChanged = 83,
  EnvelopeStaleFabricEpochAdvanced = 84,
  EnvelopeStaleNoLiveAuthority = 85,
  EnvelopeCooldownActive = 86,
  EnvelopeRecoveredRequiresRevalidation = 87,
  EnvelopeRevalidatedApplied = 88,
  EnvelopeRevalidationUnknown = 89,
  EnvelopeRevalidationDenied = 90,
  EnvelopeReducedByPolicy = 91,
  EnvelopeRevokedByOperator = 92,
  EnvelopeRevokedByGrantRecall = 93,

  // Time / clock
  ClockRegressionDetected = 100,
  ClockTickOverflowSaturated = 101,
  TokenBucketSaturated = 102,
  TokenBucketEmpty = 103,
  TokenBucketRefilled = 104,

  // Durability
  JournalAppendFailed = 110,
  JournalTruncatedTail = 111,
  JournalCorruptRecord = 112,
  JournalCommitted = 113,
  JournalCompacted = 114,
  JournalVersionUnsupported = 115,

  // Transport / backend session
  BackendSessionEstablished = 120,
  BackendSessionLost = 121,
  BackendFencedStaleEpoch = 122,
  BackendFencedStaleBoot = 123,
  BackendRejectedMalformedRequest = 124,
  BackendQueueFull = 125,

  // Governance / limits / shutdown
  CapacityLimitExceeded = 130,
  MalformedInput = 131,
  EngineShutdown = 132,
  WorkRejectedDuringShutdown = 133,
  UnsupportedOperation = 134,
  UnknownAuthority = 135,
};

enum class ReasonClass : std::uint8_t {
  None = 0,
  Progress,
  Degradation,
  Staleness,
  Fencing,
  Rejection,
  Idempotency,
  Fault,
  UnknownAuthority,
};

[[nodiscard]] std::string_view to_string_view(ReasonCode code) noexcept;
[[nodiscard]] ReasonClass classify(ReasonCode code) noexcept;
[[nodiscard]] bool is_denial(ReasonCode code) noexcept;
[[nodiscard]] bool is_staleness(ReasonCode code) noexcept;
[[nodiscard]] bool is_fencing(ReasonCode code) noexcept;

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_REASON_HPP
