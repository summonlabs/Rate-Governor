#include "rate_governor/synthetic_device.hpp"

#include <string>

#include "rate_governor/codec.hpp"
#include "rate_governor/version.hpp"

namespace rate_governor {

SyntheticDevice::SyntheticDevice(SyntheticDeviceConfig config, WorkerBootId boot,
                                 FabricEpoch epoch, WorkerBootId admitted_boot)
    : config_(config), boot_(boot), highest_epoch_(epoch), admitted_boot_(admitted_boot) {
  if (config_.envelope_limit == 0) {
    config_.envelope_limit = 1;
  }
}

bool SyntheticDevice::admit_incarnation(FabricEpoch epoch, WorkerBootId boot, ReasonCode& reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (epoch < highest_epoch_) {
    ++accounting_.fenced_stale_epoch;
    reason = ReasonCode::BackendFencedStaleEpoch;
    return false;
  }
  if (epoch == highest_epoch_) {
    if (admitted_boot_.valid() && boot != admitted_boot_) {
      // Same epoch, different incarnation: the device cannot tell which of the
      // two sessions is entitled to mutate, so it refuses rather than guesses.
      ++accounting_.fenced_stale_boot;
      reason = ReasonCode::BackendFencedStaleBoot;
      return false;
    }
    admitted_boot_ = boot;
    reason = ReasonCode::None;
    return true;
  }
  // A strictly newer epoch fences everything the previous epoch left in force.
  table_.clear();
  ++accounting_.epoch_advances;
  ++accounting_.table_clears;
  highest_epoch_ = epoch;
  admitted_boot_ = boot;
  reason = ReasonCode::None;
  return true;
}

namespace {

bool incarnation_ok(FabricEpoch request_epoch, FabricEpoch highest, WorkerBootId request_boot,
                    WorkerBootId admitted_boot, ReasonCode& reason) {
  if (request_epoch < highest) {
    reason = ReasonCode::BackendFencedStaleEpoch;
    return false;
  }
  if (request_epoch > highest) {
    // A request cannot advance the epoch by itself; the session handshake owns
    // that transition.
    reason = ReasonCode::BackendFencedStaleEpoch;
    return false;
  }
  if (request_boot != admitted_boot) {
    reason = ReasonCode::BackendFencedStaleBoot;
    return false;
  }
  return true;
}

}  // namespace

DispatchOutcome SyntheticDevice::apply(const ApplyRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  ++apply_calls_;
  DispatchOutcome outcome;
  outcome.epoch = highest_epoch_;
  outcome.boot = boot_;

  ReasonCode reason = ReasonCode::None;
  if (!incarnation_ok(request.epoch, highest_epoch_, request.boot, admitted_boot_, reason)) {
    if (reason == ReasonCode::BackendFencedStaleEpoch) {
      ++accounting_.fenced_stale_epoch;
    } else {
      ++accounting_.fenced_stale_boot;
    }
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = reason;
    outcome.detail = "device refused a request from a fenced incarnation";
    ++accounting_.refusals;
    return outcome;
  }
  if (!request.envelope.valid() || !request.envelope_generation.valid()) {
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = ReasonCode::MalformedInput;
    outcome.detail = "device refused an apply with an unusable binding";
    ++accounting_.refusals;
    return outcome;
  }
  if (config_.refuse_all_applies) {
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = ReasonCode::ApplyBackendFailed;
    outcome.detail = "device configured to refuse all applies";
    ++accounting_.refusals;
    return outcome;
  }
  if (config_.fail_every_nth_apply != 0 &&
      (apply_calls_ % config_.fail_every_nth_apply) == 0) {
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::ApplyBackendFailed;
    outcome.detail = "injected apply failure";
    ++accounting_.injection_triggers;
    return outcome;
  }
  if (config_.max_rate_ups != 0 && request.rate_ups > config_.max_rate_ups) {
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = ReasonCode::CapacityLimitExceeded;
    outcome.detail = "requested rate exceeds the declared device maximum";
    ++accounting_.refusals;
    ++accounting_.limit_rejections;
    return outcome;
  }
  if (config_.max_burst_tokens != 0 && request.burst_tokens > config_.max_burst_tokens) {
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = ReasonCode::CapacityLimitExceeded;
    outcome.detail = "requested burst exceeds the declared device maximum";
    ++accounting_.refusals;
    ++accounting_.limit_rejections;
    return outcome;
  }

  const u64 key = request.envelope.value();
  auto existing = table_.find(key);
  if (existing == table_.end() && table_.size() >= config_.envelope_limit) {
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = ReasonCode::CapacityLimitExceeded;
    outcome.detail = "device envelope table is full";
    ++accounting_.refusals;
    ++accounting_.limit_rejections;
    return outcome;
  }

  SyntheticEntry& entry = table_[key];
  entry.envelope = request.envelope;
  entry.generation = request.envelope_generation;
  entry.rate_ups = request.rate_ups;
  entry.burst_tokens = request.burst_tokens;
  entry.epoch = highest_epoch_;
  entry.boot = boot_;
  ++entry.applies;
  ++accounting_.applies;
  if (table_.size() > static_cast<usize>(accounting_.entries_peak)) {
    accounting_.entries_peak = static_cast<u64>(table_.size());
  }

  if (config_.apply_spins != 0) {
    // Deterministic busy work that widens the dispatch window. No sleeping, no
    // wall-clock dependency: the amount of work is exactly reproducible.
    volatile u64 accumulator = 0;
    for (u64 spin = 0; spin < config_.apply_spins; ++spin) {
      accumulator += spin ^ key;
    }
    (void)accumulator;
  }

  outcome.status = DispatchStatus::Completed;
  outcome.reason = ReasonCode::ApplyAcknowledged;
  outcome.applied_rate_ups = entry.rate_ups;
  outcome.applied_burst_tokens = entry.burst_tokens;
  outcome.detail = "device holds the envelope";
  return outcome;
}

DispatchOutcome SyntheticDevice::revoke(const RevokeRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  DispatchOutcome outcome;
  outcome.epoch = highest_epoch_;
  outcome.boot = boot_;

  ReasonCode reason = ReasonCode::None;
  if (!incarnation_ok(request.epoch, highest_epoch_, request.boot, admitted_boot_, reason)) {
    if (reason == ReasonCode::BackendFencedStaleEpoch) {
      ++accounting_.fenced_stale_epoch;
    } else {
      ++accounting_.fenced_stale_boot;
    }
    outcome.status = DispatchStatus::Rejected;
    outcome.reason = reason;
    outcome.detail = "device refused a revoke from a fenced incarnation";
    ++accounting_.refusals;
    return outcome;
  }
  const auto existing = table_.find(request.envelope.value());
  if (existing != table_.end()) {
    ++existing->second.revokes;
    table_.erase(existing);
  }
  ++accounting_.revokes;
  outcome.status = DispatchStatus::Completed;
  outcome.reason = ReasonCode::RevokeVerified;
  outcome.applied_rate_ups = 0;
  outcome.applied_burst_tokens = 0;
  outcome.detail = "device holds no envelope for this binding";
  return outcome;
}

VerificationOutcome SyntheticDevice::read_back(const ApplyRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  ++verify_calls_;
  VerificationOutcome outcome;
  outcome.epoch = highest_epoch_;
  outcome.boot = boot_;

  ReasonCode reason = ReasonCode::None;
  if (!incarnation_ok(request.epoch, highest_epoch_, request.boot, admitted_boot_, reason)) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = reason;
    outcome.detail = "device refused a readback from a fenced incarnation";
    return outcome;
  }
  if (config_.refuse_all_verifies ||
      (config_.refuse_every_nth_verify != 0 &&
       (verify_calls_ % config_.refuse_every_nth_verify) == 0)) {
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::ApplyVerificationUnknown;
    outcome.detail = "injected readback failure";
    ++accounting_.injection_triggers;
    return outcome;
  }
  ++accounting_.verifications;

  const auto existing = table_.find(request.envelope.value());
  if (existing == table_.end() || existing->second.generation != request.envelope_generation) {
    outcome.status = VerificationStatus::Confirmed;
    outcome.reason = ReasonCode::RevokeVerified;
    outcome.observed_present = false;
    outcome.detail = "device holds no envelope for this binding generation";
    return outcome;
  }

  outcome.status = VerificationStatus::Confirmed;
  outcome.reason = ReasonCode::ApplyVerified;
  outcome.observed_present = true;
  outcome.observed_rate_ups = existing->second.rate_ups;
  outcome.observed_burst_tokens = existing->second.burst_tokens;

  if (config_.underreport_every_nth_verify != 0 &&
      (verify_calls_ % config_.underreport_every_nth_verify) == 0) {
    outcome.observed_rate_ups = 0;
    outcome.observed_burst_tokens = 0;
    outcome.reason = ReasonCode::ApplyUnderDelivered;
    outcome.detail = "injected under-delivery readback";
    ++accounting_.under_deliveries;
    ++accounting_.injection_triggers;
  } else if (config_.overreport_every_nth_verify != 0 &&
             (verify_calls_ % config_.overreport_every_nth_verify) == 0) {
    outcome.observed_rate_ups = saturating_add(outcome.observed_rate_ups, 1);
    outcome.reason = ReasonCode::ApplyOverDeliveredRevoking;
    outcome.detail = "injected over-delivery readback";
    ++accounting_.over_deliveries;
    ++accounting_.injection_triggers;
  }
  return outcome;
}

DeviceAccounting SyntheticDevice::accounting() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return accounting_;
}

usize SyntheticDevice::entry_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return table_.size();
}

bool SyntheticDevice::lookup(RateEnvelopeId envelope, SyntheticEntry& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto existing = table_.find(envelope.value());
  if (existing == table_.end()) {
    return false;
  }
  out = existing->second;
  return true;
}

FabricEpoch SyntheticDevice::highest_epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return highest_epoch_;
}

WorkerBootId SyntheticDevice::session_boot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return boot_;
}

WorkerBootId SyntheticDevice::admitted_boot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return admitted_boot_;
}

void SyntheticDevice::force_clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  table_.clear();
  ++accounting_.table_clears;
}

// ---------------------------------------------------------------------------
// SyntheticBackend
// ---------------------------------------------------------------------------
SyntheticBackend::SyntheticBackend(BackendId id, Generation generation,
                                   SyntheticDeviceConfig config, WorkerBootId boot,
                                   FabricEpoch epoch, std::string label)
    // In-process: the coordinating process is the enforcement process.
    : device_(config, boot, epoch, boot) {
  descriptor_.id = id;
  descriptor_.generation = generation;
  descriptor_.kind = BackendKind::Synthetic;
  descriptor_.capabilities = kBackendCapApply | kBackendCapRevoke | kBackendCapVerify |
                             kBackendCapBurst;
  descriptor_.verification = VerificationMode::PostApplyReadback;
  descriptor_.max_rate_ups = config.max_rate_ups;
  descriptor_.max_burst_tokens = config.max_burst_tokens;
  descriptor_.label = truncate_bounded(std::move(label), kMaxProvenanceDetail);
  descriptor_.provenance.source = AuthoritySource::Configuration;
  descriptor_.provenance.epoch = epoch;
  descriptor_.provenance.boot = boot;
  descriptor_.provenance.detail = "synthetic software enforcement device";
}

BackendDescriptor SyntheticBackend::describe() const { return descriptor_; }

DispatchOutcome SyntheticBackend::dispatch_apply(const ApplyRequest& request) {
  if (!live_) {
    DispatchOutcome outcome;
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = "backend session is not live";
    return outcome;
  }
  return device_.apply(request);
}

DispatchOutcome SyntheticBackend::dispatch_revoke(const RevokeRequest& request) {
  if (!live_) {
    DispatchOutcome outcome;
    outcome.status = DispatchStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = "backend session is not live";
    return outcome;
  }
  return device_.revoke(request);
}

VerificationOutcome SyntheticBackend::verify(const ApplyRequest& request) {
  if (!live_) {
    VerificationOutcome outcome;
    outcome.status = VerificationStatus::Failed;
    outcome.reason = ReasonCode::BackendSessionLost;
    outcome.detail = "backend session is not live; effect is UNKNOWN";
    return outcome;
  }
  return device_.read_back(request);
}

std::string SyntheticBackend::session_description() const {
  std::string out = "in-process synthetic device (";
  out.append(kProductName);
  out.append(" ");
  out.append(kVersionString);
  out.append("); no physical shaping is performed or claimed");
  return out;
}

}  // namespace rate_governor
