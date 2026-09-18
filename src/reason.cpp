#include "rate_governor/reason.hpp"

namespace rate_governor {

std::string_view to_string_view(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::None: return "None";

    case ReasonCode::PlanAuthorized: return "PlanAuthorized";
    case ReasonCode::PlanTargetClampedByCeiling: return "PlanTargetClampedByCeiling";
    case ReasonCode::PlanBurstClampedByAuthority: return "PlanBurstClampedByAuthority";
    case ReasonCode::PlanRejectedFloorExceedsFundedCeiling: return "PlanRejectedFloorExceedsFundedCeiling";
    case ReasonCode::PlanRejectedTargetBelowFloor: return "PlanRejectedTargetBelowFloor";
    case ReasonCode::PlanRejectedUnknownFlow: return "PlanRejectedUnknownFlow";
    case ReasonCode::PlanRejectedUnknownResource: return "PlanRejectedUnknownResource";
    case ReasonCode::PlanRejectedMissingGrant: return "PlanRejectedMissingGrant";
    case ReasonCode::PlanRejectedMissingPolicy: return "PlanRejectedMissingPolicy";
    case ReasonCode::PlanRejectedMissingReservation: return "PlanRejectedMissingReservation";
    case ReasonCode::PlanRejectedMissingBackend: return "PlanRejectedMissingBackend";
    case ReasonCode::PlanRejectedFlowGenerationMismatch: return "PlanRejectedFlowGenerationMismatch";
    case ReasonCode::PlanRejectedGrantGenerationMismatch: return "PlanRejectedGrantGenerationMismatch";
    case ReasonCode::PlanRejectedReservationGenerationMismatch: return "PlanRejectedReservationGenerationMismatch";
    case ReasonCode::PlanRejectedPolicyGenerationMismatch: return "PlanRejectedPolicyGenerationMismatch";
    case ReasonCode::PlanRejectedResourceGenerationMismatch: return "PlanRejectedResourceGenerationMismatch";
    case ReasonCode::PlanRejectedBackendGenerationMismatch: return "PlanRejectedBackendGenerationMismatch";
    case ReasonCode::PlanRejectedGrantNotActive: return "PlanRejectedGrantNotActive";
    case ReasonCode::PlanRejectedPolicyNotActive: return "PlanRejectedPolicyNotActive";
    case ReasonCode::PlanRejectedResourceNotActive: return "PlanRejectedResourceNotActive";
    case ReasonCode::PlanRejectedReservationNotActive: return "PlanRejectedReservationNotActive";
    case ReasonCode::PlanRejectedGrantNotYetValid: return "PlanRejectedGrantNotYetValid";
    case ReasonCode::PlanRejectedGrantExpired: return "PlanRejectedGrantExpired";
    case ReasonCode::PlanRejectedReservationNotYetValid: return "PlanRejectedReservationNotYetValid";
    case ReasonCode::PlanRejectedReservationExpired: return "PlanRejectedReservationExpired";
    case ReasonCode::PlanRejectedPolicyExpired: return "PlanRejectedPolicyExpired";
    case ReasonCode::PlanRejectedPolicyNotYetValid: return "PlanRejectedPolicyNotYetValid";
    case ReasonCode::PlanRejectedBackendUnsupported: return "PlanRejectedBackendUnsupported";
    case ReasonCode::PlanRejectedEnvelopeGenerationMismatch: return "PlanRejectedEnvelopeGenerationMismatch";
    case ReasonCode::PlanRejectedResourceCapacityZero: return "PlanRejectedResourceCapacityZero";
    case ReasonCode::PlanRejectedCeilingZero: return "PlanRejectedCeilingZero";
    case ReasonCode::PlanRejectedFlowNotActive: return "PlanRejectedFlowNotActive";

    case ReasonCode::ApplyDispatched: return "ApplyDispatched";
    case ReasonCode::ApplyAcknowledged: return "ApplyAcknowledged";
    case ReasonCode::ApplyVerified: return "ApplyVerified";
    case ReasonCode::ApplyUnderDelivered: return "ApplyUnderDelivered";
    case ReasonCode::ApplyOverDeliveredRevoking: return "ApplyOverDeliveredRevoking";
    case ReasonCode::ApplyBackendFailed: return "ApplyBackendFailed";
    case ReasonCode::ApplyVerificationUnsupported: return "ApplyVerificationUnsupported";
    case ReasonCode::ApplyVerificationMismatch: return "ApplyVerificationMismatch";
    case ReasonCode::ApplyVerificationUnknown: return "ApplyVerificationUnknown";
    case ReasonCode::RevokeDispatched: return "RevokeDispatched";
    case ReasonCode::RevokeVerified: return "RevokeVerified";
    case ReasonCode::RevokeBackendFailed: return "RevokeBackendFailed";
    case ReasonCode::AttemptCancelled: return "AttemptCancelled";
    case ReasonCode::AttemptLateCompletionRejected: return "AttemptLateCompletionRejected";
    case ReasonCode::AttemptDuplicateIgnored: return "AttemptDuplicateIgnored";
    case ReasonCode::AttemptAbandonedWorkerDeath: return "AttemptAbandonedWorkerDeath";
    case ReasonCode::AttemptDeadlineExceeded: return "AttemptDeadlineExceeded";
    case ReasonCode::AttemptFencedByEpoch: return "AttemptFencedByEpoch";
    case ReasonCode::AttemptFencedByBoot: return "AttemptFencedByBoot";
    case ReasonCode::AttemptFencedByGeneration: return "AttemptFencedByGeneration";
    case ReasonCode::AttemptSupersededByNewerAttempt: return "AttemptSupersededByNewerAttempt";
    case ReasonCode::AttemptIdempotentReplay: return "AttemptIdempotentReplay";
    case ReasonCode::CompensatingRevokeIssued: return "CompensatingRevokeIssued";

    case ReasonCode::EnvelopeOpened: return "EnvelopeOpened";
    case ReasonCode::EnvelopeAuthorized: return "EnvelopeAuthorized";
    case ReasonCode::EnvelopeRevokePending: return "EnvelopeRevokePending";
    case ReasonCode::EnvelopeRevoked: return "EnvelopeRevoked";
    case ReasonCode::EnvelopeDegraded: return "EnvelopeDegraded";
    case ReasonCode::EnvelopeFailed: return "EnvelopeFailed";
    case ReasonCode::EnvelopeStaleGrantRecalled: return "EnvelopeStaleGrantRecalled";
    case ReasonCode::EnvelopeStaleGrantExpired: return "EnvelopeStaleGrantExpired";
    case ReasonCode::EnvelopeStaleReservationExpired: return "EnvelopeStaleReservationExpired";
    case ReasonCode::EnvelopeStalePolicyExpired: return "EnvelopeStalePolicyExpired";
    case ReasonCode::EnvelopeStalePolicyGenerationChanged: return "EnvelopeStalePolicyGenerationChanged";
    case ReasonCode::EnvelopeStaleResourceGenerationChanged: return "EnvelopeStaleResourceGenerationChanged";
    case ReasonCode::EnvelopeStaleResourceCapacityReduced: return "EnvelopeStaleResourceCapacityReduced";
    case ReasonCode::EnvelopeStaleFlowGenerationChanged: return "EnvelopeStaleFlowGenerationChanged";
    case ReasonCode::EnvelopeStaleFabricEpochAdvanced: return "EnvelopeStaleFabricEpochAdvanced";
    case ReasonCode::EnvelopeStaleNoLiveAuthority: return "EnvelopeStaleNoLiveAuthority";
    case ReasonCode::EnvelopeCooldownActive: return "EnvelopeCooldownActive";
    case ReasonCode::EnvelopeRecoveredRequiresRevalidation: return "EnvelopeRecoveredRequiresRevalidation";
    case ReasonCode::EnvelopeRevalidatedApplied: return "EnvelopeRevalidatedApplied";
    case ReasonCode::EnvelopeRevalidationUnknown: return "EnvelopeRevalidationUnknown";
    case ReasonCode::EnvelopeRevalidationDenied: return "EnvelopeRevalidationDenied";
    case ReasonCode::EnvelopeReducedByPolicy: return "EnvelopeReducedByPolicy";
    case ReasonCode::EnvelopeRevokedByOperator: return "EnvelopeRevokedByOperator";
    case ReasonCode::EnvelopeRevokedByGrantRecall: return "EnvelopeRevokedByGrantRecall";

    case ReasonCode::ClockRegressionDetected: return "ClockRegressionDetected";
    case ReasonCode::ClockTickOverflowSaturated: return "ClockTickOverflowSaturated";
    case ReasonCode::TokenBucketSaturated: return "TokenBucketSaturated";
    case ReasonCode::TokenBucketEmpty: return "TokenBucketEmpty";
    case ReasonCode::TokenBucketRefilled: return "TokenBucketRefilled";

    case ReasonCode::JournalAppendFailed: return "JournalAppendFailed";
    case ReasonCode::JournalTruncatedTail: return "JournalTruncatedTail";
    case ReasonCode::JournalCorruptRecord: return "JournalCorruptRecord";
    case ReasonCode::JournalCommitted: return "JournalCommitted";
    case ReasonCode::JournalCompacted: return "JournalCompacted";
    case ReasonCode::JournalVersionUnsupported: return "JournalVersionUnsupported";

    case ReasonCode::BackendSessionEstablished: return "BackendSessionEstablished";
    case ReasonCode::BackendSessionLost: return "BackendSessionLost";
    case ReasonCode::BackendFencedStaleEpoch: return "BackendFencedStaleEpoch";
    case ReasonCode::BackendFencedStaleBoot: return "BackendFencedStaleBoot";
    case ReasonCode::BackendRejectedMalformedRequest: return "BackendRejectedMalformedRequest";
    case ReasonCode::BackendQueueFull: return "BackendQueueFull";

    case ReasonCode::CapacityLimitExceeded: return "CapacityLimitExceeded";
    case ReasonCode::MalformedInput: return "MalformedInput";
    case ReasonCode::EngineShutdown: return "EngineShutdown";
    case ReasonCode::WorkRejectedDuringShutdown: return "WorkRejectedDuringShutdown";
    case ReasonCode::UnsupportedOperation: return "UnsupportedOperation";
    case ReasonCode::UnknownAuthority: return "UnknownAuthority";
  }
  return "Unknown";
}

ReasonClass classify(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::None:
      return ReasonClass::None;
    case ReasonCode::PlanAuthorized:
    case ReasonCode::PlanTargetClampedByCeiling:
    case ReasonCode::PlanBurstClampedByAuthority:
    case ReasonCode::EnvelopeOpened:
    case ReasonCode::EnvelopeAuthorized:
    case ReasonCode::ApplyDispatched:
    case ReasonCode::ApplyAcknowledged:
    case ReasonCode::ApplyVerified:
    case ReasonCode::RevokeDispatched:
    case ReasonCode::RevokeVerified:
    case ReasonCode::EnvelopeRevoked:
    case ReasonCode::EnvelopeRevalidatedApplied:
    case ReasonCode::TokenBucketRefilled:
    case ReasonCode::JournalCommitted:
    case ReasonCode::JournalCompacted:
    case ReasonCode::BackendSessionEstablished:
    case ReasonCode::EnvelopeReducedByPolicy:
      return ReasonClass::Progress;

    case ReasonCode::ApplyUnderDelivered:
    case ReasonCode::ApplyOverDeliveredRevoking:
    case ReasonCode::EnvelopeDegraded:
    case ReasonCode::EnvelopeRevokePending:
    case ReasonCode::EnvelopeCooldownActive:
    case ReasonCode::EnvelopeRevalidationUnknown:
    case ReasonCode::EnvelopeStaleResourceCapacityReduced:
    case ReasonCode::TokenBucketSaturated:
    case ReasonCode::TokenBucketEmpty:
    case ReasonCode::ClockTickOverflowSaturated:
      return ReasonClass::Degradation;

    case ReasonCode::EnvelopeStaleGrantRecalled:
    case ReasonCode::EnvelopeStaleGrantExpired:
    case ReasonCode::EnvelopeStaleReservationExpired:
    case ReasonCode::EnvelopeStalePolicyExpired:
    case ReasonCode::EnvelopeStalePolicyGenerationChanged:
    case ReasonCode::EnvelopeStaleResourceGenerationChanged:
    case ReasonCode::EnvelopeStaleFlowGenerationChanged:
    case ReasonCode::EnvelopeStaleFabricEpochAdvanced:
    case ReasonCode::EnvelopeStaleNoLiveAuthority:
      return ReasonClass::Staleness;

    case ReasonCode::AttemptFencedByEpoch:
    case ReasonCode::AttemptFencedByBoot:
    case ReasonCode::AttemptFencedByGeneration:
    case ReasonCode::AttemptSupersededByNewerAttempt:
    case ReasonCode::BackendFencedStaleEpoch:
    case ReasonCode::BackendFencedStaleBoot:
      return ReasonClass::Fencing;

    case ReasonCode::AttemptCancelled:
    case ReasonCode::AttemptLateCompletionRejected:
    case ReasonCode::AttemptDuplicateIgnored:
    case ReasonCode::AttemptIdempotentReplay:
      return ReasonClass::Idempotency;

    case ReasonCode::PlanRejectedFloorExceedsFundedCeiling:
    case ReasonCode::PlanRejectedTargetBelowFloor:
    case ReasonCode::PlanRejectedUnknownFlow:
    case ReasonCode::PlanRejectedUnknownResource:
    case ReasonCode::PlanRejectedMissingGrant:
    case ReasonCode::PlanRejectedMissingPolicy:
    case ReasonCode::PlanRejectedMissingReservation:
    case ReasonCode::PlanRejectedMissingBackend:
    case ReasonCode::PlanRejectedFlowGenerationMismatch:
    case ReasonCode::PlanRejectedGrantGenerationMismatch:
    case ReasonCode::PlanRejectedReservationGenerationMismatch:
    case ReasonCode::PlanRejectedPolicyGenerationMismatch:
    case ReasonCode::PlanRejectedResourceGenerationMismatch:
    case ReasonCode::PlanRejectedBackendGenerationMismatch:
    case ReasonCode::PlanRejectedGrantNotActive:
    case ReasonCode::PlanRejectedPolicyNotActive:
    case ReasonCode::PlanRejectedResourceNotActive:
    case ReasonCode::PlanRejectedReservationNotActive:
    case ReasonCode::PlanRejectedGrantNotYetValid:
    case ReasonCode::PlanRejectedGrantExpired:
    case ReasonCode::PlanRejectedReservationNotYetValid:
    case ReasonCode::PlanRejectedReservationExpired:
    case ReasonCode::PlanRejectedPolicyExpired:
    case ReasonCode::PlanRejectedPolicyNotYetValid:
    case ReasonCode::PlanRejectedBackendUnsupported:
    case ReasonCode::PlanRejectedEnvelopeGenerationMismatch:
    case ReasonCode::PlanRejectedResourceCapacityZero:
    case ReasonCode::PlanRejectedCeilingZero:
    case ReasonCode::PlanRejectedFlowNotActive:
    case ReasonCode::EnvelopeFailed:
    case ReasonCode::EnvelopeRevalidationDenied:
    case ReasonCode::CapacityLimitExceeded:
    case ReasonCode::MalformedInput:
    case ReasonCode::WorkRejectedDuringShutdown:
    case ReasonCode::UnsupportedOperation:
    case ReasonCode::BackendRejectedMalformedRequest:
    case ReasonCode::BackendQueueFull:
    case ReasonCode::EnvelopeRevokedByOperator:
    case ReasonCode::EnvelopeRevokedByGrantRecall:
      return ReasonClass::Rejection;

    case ReasonCode::ApplyBackendFailed:
    case ReasonCode::ApplyVerificationUnsupported:
    case ReasonCode::ApplyVerificationMismatch:
    case ReasonCode::ApplyVerificationUnknown:
    case ReasonCode::RevokeBackendFailed:
    case ReasonCode::AttemptAbandonedWorkerDeath:
    case ReasonCode::AttemptDeadlineExceeded:
    case ReasonCode::ClockRegressionDetected:
    case ReasonCode::JournalAppendFailed:
    case ReasonCode::JournalTruncatedTail:
    case ReasonCode::JournalCorruptRecord:
    case ReasonCode::JournalVersionUnsupported:
    case ReasonCode::BackendSessionLost:
    case ReasonCode::EngineShutdown:
    case ReasonCode::CompensatingRevokeIssued:
    case ReasonCode::EnvelopeRecoveredRequiresRevalidation:
      return ReasonClass::Fault;

    case ReasonCode::UnknownAuthority:
      break;
  }
  return ReasonClass::UnknownAuthority;
}

bool is_denial(ReasonCode code) noexcept { return classify(code) == ReasonClass::Rejection; }

bool is_staleness(ReasonCode code) noexcept { return classify(code) == ReasonClass::Staleness; }

bool is_fencing(ReasonCode code) noexcept { return classify(code) == ReasonClass::Fencing; }

}  // namespace rate_governor
