#include "rate_governor/backend.hpp"

#include "rate_governor/codec.hpp"

namespace rate_governor {

IEnforcementBackend::~IEnforcementBackend() = default;

UnsupportedBackend::UnsupportedBackend(BackendId id, Generation generation, std::string label) {
  descriptor_.id = id;
  descriptor_.generation = generation;
  descriptor_.kind = BackendKind::Unsupported;
  descriptor_.capabilities = 0;
  descriptor_.verification = VerificationMode::None;
  descriptor_.max_rate_ups = 0;
  descriptor_.max_burst_tokens = 0;
  descriptor_.label = truncate_bounded(std::move(label), kMaxProvenanceDetail);
  descriptor_.provenance.source = AuthoritySource::Configuration;
  descriptor_.provenance.detail = "unsupported backend";
}

BackendDescriptor UnsupportedBackend::describe() const { return descriptor_; }

DispatchOutcome UnsupportedBackend::dispatch_apply(const ApplyRequest& request) {
  DispatchOutcome outcome;
  outcome.status = DispatchStatus::Unsupported;
  outcome.reason = ReasonCode::PlanRejectedBackendUnsupported;
  outcome.epoch = request.epoch;
  outcome.boot = request.boot;
  outcome.detail = "backend cannot apply a rate envelope";
  return outcome;
}

DispatchOutcome UnsupportedBackend::dispatch_revoke(const RevokeRequest& request) {
  DispatchOutcome outcome;
  outcome.status = DispatchStatus::Unsupported;
  outcome.reason = ReasonCode::UnsupportedOperation;
  outcome.epoch = request.epoch;
  outcome.boot = request.boot;
  outcome.detail = "backend cannot revoke a rate envelope";
  return outcome;
}

VerificationOutcome UnsupportedBackend::verify(const ApplyRequest& request) {
  VerificationOutcome outcome;
  outcome.status = VerificationStatus::Unsupported;
  outcome.reason = ReasonCode::ApplyVerificationUnsupported;
  outcome.epoch = request.epoch;
  outcome.boot = request.boot;
  outcome.detail = "backend has no post-apply readback";
  return outcome;
}

}  // namespace rate_governor
