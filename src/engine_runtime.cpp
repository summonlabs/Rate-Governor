#include "rate_governor/engine.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "rate_governor/codec.hpp"
#include "rate_governor/plan.hpp"
#include "engine_lock.hpp"
#include "record_codec.hpp"

namespace rate_governor {
namespace {

inline constexpr usize kMaxTickEnvelopeTransitions = 512;
inline constexpr usize kMaxTickAttemptTransitions = 512;
inline constexpr usize kMaxRevalidationsPerCall = 256;

bool attempt_is_inflight(AttemptState state) {
  return state == AttemptState::Created || state == AttemptState::Dispatched ||
         state == AttemptState::Acknowledged;
}

void merge_envelope(std::vector<RateEnvelope>& list, const RateEnvelope& candidate) {
  for (RateEnvelope& existing : list) {
    if (existing.id == candidate.id) {
      existing = candidate;
      return;
    }
  }
  list.push_back(candidate);
}

void merge_attempt(std::vector<EnforcementAttempt>& list, const EnforcementAttempt& candidate) {
  for (EnforcementAttempt& existing : list) {
    if (existing.id == candidate.id) {
      existing = candidate;
      return;
    }
  }
  list.push_back(candidate);
}

}  // namespace

TickReport RateGovernor::tick() {
  const TimestampNs now = clock_->NowNs();
  TickReport report;
  report.now_ns = now;
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (tick_seen_ && now < last_tick_ns_) {
      ++counters_.clock_regressions;
      ++report.clock_regressions;
      push_event_locked(ReasonCode::ClockRegressionDetected, RateEnvelopeId{}, Generation{},
                        EnforcementAttemptId{},
                        "the supplied clock moved backwards; elapsed time was treated as zero",
                        events);
    }
    last_tick_ns_ = now;
    tick_seen_ = true;

    std::vector<RateEnvelope> envelope_updates;
    std::vector<EnforcementAttempt> attempt_updates;

    if (backend_ != nullptr && backend_bound_ && !backend_->session_live()) {
      // Maintenance notices the loss before anything else tries to use it.
      std::vector<RateEnvelope> lost;
      collect_session_lost_updates_locked(lost);
      backend_bound_ = false;
      for (const RateEnvelope& envelope : lost) {
        merge_envelope(envelope_updates, envelope);
      }
      push_event_locked(ReasonCode::BackendSessionLost, RateEnvelopeId{}, Generation{},
                        EnforcementAttemptId{},
                        "the enforcement session is gone; every effect claim it supported now "
                        "requires revalidation",
                        events);
    }

    for (auto& entry : envelopes_) {
      RateEnvelope updated = entry.second;
      bool changed = false;

      if (updated.bucket.initialized) {
        const RefillOutcome refill = refill_bucket(updated.bucket, now);
        if (refill.clock_regressed) {
          ++report.clock_regressions;
          ++counters_.clock_regressions;
          push_event_locked(ReasonCode::ClockRegressionDetected, updated.id, updated.generation,
                            EnforcementAttemptId{},
                            "bucket refill observed a backwards clock; no tokens were minted",
                            events);
        }
        if (refill.saturated) {
          ++report.token_buckets_saturated;
        }
        if (refill.tokens_granted != 0) {
          ++report.envelopes_refilled;
          changed = true;
        } else if (refill.elapsed_ns != 0) {
          changed = true;
        }
      }

      if (updated.cooldown_until_ns != 0 && now >= updated.cooldown_until_ns) {
        updated.cooldown_until_ns = 0;
        ++report.cooldowns_expired;
        if (updated.state == EnvelopeState::Degraded && !updated.requires_revalidation) {
          updated.state = EnvelopeState::Authorized;
          updated.reason = ReasonCode::EnvelopeAuthorized;
        }
        changed = true;
      }

      const ReasonCode stale = authority_status_locked(updated, now);
      if (stale != ReasonCode::None && updated.reason != stale &&
          updated.state != EnvelopeState::Revoked && updated.state != EnvelopeState::Stale) {
        const bool holds_effect =
            updated.state == EnvelopeState::Applied || updated.state == EnvelopeState::Degraded ||
            updated.state == EnvelopeState::Dispatching ||
            updated.state == EnvelopeState::RevokePending || updated.effect_verified ||
            updated.applied_rate_ups != 0;
        updated.requires_revalidation = holds_effect || updated.requires_revalidation;
        updated.effect_verified = false;
        updated.state = holds_effect ? EnvelopeState::RevokePending : EnvelopeState::Stale;
        updated.reason = stale;
        ++report.envelopes_stale;
        if (holds_effect) {
          ++report.revokes_pending;
        }
        ++counters_.stale_transitions;
        changed = true;
        push_event_locked(stale, updated.id, updated.generation, EnforcementAttemptId{},
                          "the authority that funded this envelope is no longer current", events);
      }

      if (changed) {
        if (envelope_updates.size() >= kMaxTickEnvelopeTransitions) {
          ++report.deferred;
        } else {
          updated.updated_ns = now;
          merge_envelope(envelope_updates, updated);
        }
      }
    }

    for (auto& entry : attempts_) {
      EnforcementAttempt& attempt = entry.second;
      if (!attempt_is_inflight(attempt.state)) {
        continue;
      }
      if (attempt.deadline_ns == 0 || now <= attempt.deadline_ns) {
        continue;
      }
      if (attempt_updates.size() >= kMaxTickAttemptTransitions) {
        ++report.deferred;
        continue;
      }
      EnforcementAttempt updated = attempt;
      updated.state = AttemptState::Ambiguous;
      updated.reason = ReasonCode::AttemptDeadlineExceeded;
      updated.completed_ns = now;
      updated.detail = "attempt deadline passed without a completion; the outcome is UNKNOWN";
      merge_attempt(attempt_updates, updated);
      ++report.attempts_expired;

      const auto found = envelopes_.find(updated.envelope.value());
      if (found != envelopes_.end()) {
        RateEnvelope envelope_update = found->second;
        envelope_update.state = EnvelopeState::Degraded;
        envelope_update.reason = ReasonCode::AttemptDeadlineExceeded;
        envelope_update.requires_revalidation = true;
        envelope_update.effect_verified = false;
        envelope_update.updated_ns = now;
        merge_envelope(envelope_updates, envelope_update);
        ++report.revalidations_required;
      }
      push_event_locked(ReasonCode::AttemptDeadlineExceeded, updated.envelope,
                        updated.envelope_generation, updated.id, updated.detail, events);
    }

    if (!envelope_updates.empty() || !attempt_updates.empty()) {
      StagedRecords records;
      for (const RateEnvelope& envelope : envelope_updates) {
        stage_envelope(records, envelope, JournalRecordType::EnvelopeStateChange);
      }
      for (const EnforcementAttempt& attempt : attempt_updates) {
        stage_attempt(records, attempt, JournalRecordType::AttemptTerminated);
      }
      const Status status = commit_many_locked(records);
      if (!status) {
        report.journal_failed = true;
        report.deferred += envelope_updates.size() + attempt_updates.size();
      } else {
        for (const RateEnvelope& envelope : envelope_updates) {
          envelopes_[envelope.id.value()] = envelope;
        }
        for (const EnforcementAttempt& attempt : attempt_updates) {
          attempts_[attempt.id.value()] = attempt;
        }
      }
    }

    for (const auto& entry : envelopes_) {
      if (entry.second.requires_revalidation &&
          entry.second.state != EnvelopeState::Revoked) {
        ++report.revalidations_required;
      }
    }
  }
  emit(events);
  return report;
}

RevalidationReport RateGovernor::revalidate_all(const ActorContext& actor) {
  RevalidationReport report;
  std::vector<u64> candidates;
  bool backend_ready = false;
  IEnforcementBackend* backend = nullptr;
  FabricEpoch epoch_now{};
  TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      report.explanation = std::string("revalidation refused: ") +
                           std::string(to_string_view(fence));
      emit(events);
      return report;
    }
    const TimestampNs selection_now = clock_->NowNs();
    for (const auto& entry : envelopes_) {
      const RateEnvelope& envelope = entry.second;
      if (envelope.state == EnvelopeState::Revoked) {
        continue;
      }
      // An envelope is revalidated when a claim has already been questioned, or
      // when its scheduled revalidation horizon has passed: a verified effect is
      // evidence about the moment it was read back, not a permanent fact.
      const bool scheduled =
          envelope.state == EnvelopeState::Applied && envelope.revalidate_by_ns != 0 &&
          selection_now >= envelope.revalidate_by_ns;
      if (!envelope.requires_revalidation && !scheduled) {
        continue;
      }
      if (candidates.size() < kMaxRevalidationsPerCall) {
        candidates.push_back(entry.first);
      } else {
        ++report.skipped_no_backend;
      }
    }
    backend = backend_;
    backend_ready = backend_ != nullptr && backend_bound_;
    epoch_now = epoch_;
  }
  report.considered = static_cast<u64>(candidates.size());
  if (!backend_ready) {
    report.skipped_no_backend += report.considered;
    report.explanation =
        "no backend session is admitted at the current fabric epoch; every effect claim stays "
        "UNKNOWN";
    emit(events);
    return report;
  }

  for (const u64 raw_id : candidates) {
    const RateEnvelopeId id(raw_id);
    ApplyRequest probe;
    EnvelopeState before = EnvelopeState::Unknown;
    u64 expected_rate = 0;
    u64 expected_burst = 0;
    u64 floor = 0;
    {
      ScopedLock guard(mutex_);
      const RateEnvelope* envelope = find_envelope_locked(id);
      if (envelope == nullptr) {
        continue;
      }
      const bool scheduled = envelope->state == EnvelopeState::Applied &&
                             envelope->revalidate_by_ns != 0 &&
                             clock_->NowNs() >= envelope->revalidate_by_ns;
      if (!envelope->requires_revalidation && !scheduled) {
        continue;
      }
      before = envelope->state;
      expected_rate = envelope->plan.ceiling_ups();
      expected_burst = envelope->plan.burst_tokens();
      floor = envelope->plan.floor_ups();
      probe.envelope = envelope->id;
      probe.envelope_generation = envelope->generation;
      probe.fingerprint = envelope->plan.fingerprint;
      probe.rate_ups = expected_rate;
      probe.burst_tokens = expected_burst;
      probe.refill = envelope->plan.refill;
      probe.epoch = epoch_now;
      probe.boot = backend->session_boot();
    }

    const VerificationOutcome verification = backend->verify(probe);
    now = clock_->NowNs();

    RevalidationItem item;
    item.envelope = id;
    item.before = before;
    {
      ScopedLock guard(mutex_);
      RateEnvelope* envelope = find_envelope_locked(id);
      if (envelope == nullptr) {
        continue;
      }
      const bool still_scheduled = envelope->state == EnvelopeState::Applied &&
                                   envelope->revalidate_by_ns != 0 &&
                                   clock_->NowNs() >= envelope->revalidate_by_ns;
      if (!envelope->requires_revalidation && !still_scheduled) {
        continue;
      }
      RateEnvelope updated = *envelope;
      if (verification.status == VerificationStatus::Unsupported ||
          verification.status == VerificationStatus::Failed) {
        updated.reason = ReasonCode::EnvelopeRevalidationUnknown;
        updated.requires_revalidation = true;
        updated.effect_verified = false;
        ++report.unknown;
        item.reason = ReasonCode::EnvelopeRevalidationUnknown;
      } else if (!verification.observed_present) {
        // Nothing is enforced at the backend: the envelope must be re-applied.
        updated.state = EnvelopeState::Authorized;
        updated.reason = ReasonCode::EnvelopeRevalidationDenied;
        updated.requires_revalidation = false;
        updated.effect_verified = false;
        updated.applied_rate_ups = 0;
        updated.applied_burst_tokens = 0;
        updated.applied_epoch = FabricEpoch{};
        updated.applied_boot = WorkerBootId{};
        ++report.requeued;
        item.reason = ReasonCode::EnvelopeRevalidationDenied;
        item.reapply_required = true;
      } else if (verification.observed_rate_ups == expected_rate &&
                 verification.observed_burst_tokens == expected_burst) {
        updated.state = EnvelopeState::Applied;
        updated.reason = ReasonCode::EnvelopeRevalidatedApplied;
        updated.requires_revalidation = false;
        updated.effect_verified = true;
        updated.applied_rate_ups = verification.observed_rate_ups;
        updated.applied_burst_tokens = verification.observed_burst_tokens;
        updated.applied_epoch = verification.epoch.valid() ? verification.epoch : epoch_now;
        updated.applied_boot = verification.boot;
        ++report.confirmed_applied;
        item.reason = ReasonCode::EnvelopeRevalidatedApplied;
        item.effect_confirmed = true;
      } else {
        const bool over = verification.observed_rate_ups > expected_rate;
        updated.state = over ? EnvelopeState::RevokePending : EnvelopeState::Degraded;
        updated.reason = over ? ReasonCode::ApplyOverDeliveredRevoking
                              : (verification.observed_rate_ups >= floor
                                     ? ReasonCode::ApplyUnderDelivered
                                     : ReasonCode::EnvelopeRevalidationDenied);
        updated.requires_revalidation = true;
        updated.effect_verified = false;
        updated.applied_rate_ups = verification.observed_rate_ups;
        updated.applied_burst_tokens = verification.observed_burst_tokens;
        updated.applied_epoch = verification.epoch.valid() ? verification.epoch : epoch_now;
        updated.applied_boot = verification.boot;
        ++report.denied;
        item.reason = updated.reason;
      }
      updated.updated_ns = now;
      StagedRecords records;
      stage_envelope(records, updated, JournalRecordType::EnvelopeStateChange);
      const Status status = commit_records_locked(records);
      if (!status) {
        item.reason = ReasonCode::JournalAppendFailed;
        item.after = envelope->state;
        item.explanation = status.message();
        report.items.push_back(item);
        continue;
      }
      *envelope = updated;
      item.after = updated.state;
      item.explanation = std::string("revalidation readback: present=") +
                         (verification.observed_present ? "true" : "false") + " observed=" +
                         std::to_string(verification.observed_rate_ups) + " expected=" +
                         std::to_string(expected_rate);
      push_event_locked(item.reason, id, updated.generation, EnforcementAttemptId{},
                        item.explanation, events);
    }
    report.items.push_back(item);
  }

  if (report.explanation.empty()) {
    report.explanation = "revalidation readback completed for " +
                         std::to_string(report.considered) + " envelope(s)";
  }
  emit(events);
  return report;
}

ShutdownReport RateGovernor::shutdown() {
  ShutdownReport report;
  std::vector<AuditEvent> events;
  IEnforcementBackend* backend = nullptr;
  {
    ScopedLock guard(mutex_);
    shutting_down_.store(true, std::memory_order_release);
    report.work_stopped = true;

    std::vector<EnforcementAttempt> attempt_updates;
    std::vector<RateEnvelope> envelope_updates;
    const TimestampNs now = clock_->NowNs();

    for (auto& entry : attempts_) {
      if (!attempt_is_inflight(entry.second.state)) {
        continue;
      }
      EnforcementAttempt updated = entry.second;
      updated.state = AttemptState::Abandoned;
      updated.reason = ReasonCode::EngineShutdown;
      updated.completed_ns = now;
      updated.detail =
          "engine shutdown while the attempt was in flight; the effect is UNKNOWN";
      merge_attempt(attempt_updates, updated);
      ++report.attempts_abandoned;
      const auto found = envelopes_.find(updated.envelope.value());
      if (found != envelopes_.end()) {
        RateEnvelope envelope_update = found->second;
        envelope_update.state = EnvelopeState::Degraded;
        envelope_update.reason = ReasonCode::EngineShutdown;
        envelope_update.requires_revalidation = true;
        envelope_update.effect_verified = false;
        envelope_update.updated_ns = now;
        merge_envelope(envelope_updates, envelope_update);
      }
      push_event_locked(ReasonCode::EngineShutdown, updated.envelope, updated.envelope_generation,
                        updated.id, updated.detail, events);
    }

    if (!attempt_updates.empty() || !envelope_updates.empty()) {
      StagedRecords records;
      for (const EnforcementAttempt& attempt : attempt_updates) {
        stage_attempt(records, attempt, JournalRecordType::AttemptTerminated);
      }
      for (const RateEnvelope& envelope : envelope_updates) {
        stage_envelope(records, envelope, JournalRecordType::EnvelopeStateChange);
      }
      const Status status = commit_many_locked(records);
      if (status) {
        for (const EnforcementAttempt& attempt : attempt_updates) {
          attempts_[attempt.id.value()] = attempt;
        }
        for (const RateEnvelope& envelope : envelope_updates) {
          envelopes_[envelope.id.value()] = envelope;
        }
      } else {
        report.explanation = std::string("shutdown accounting could not be persisted: ") +
                             status.message();
      }
    }

    for (const auto& entry : envelopes_) {
      if (entry.second.requires_revalidation && entry.second.state != EnvelopeState::Revoked) {
        ++report.envelopes_requiring_revalidation;
      }
    }
    report.events_dropped = events_dropped_;
    backend = backend_;
  }
  emit(events);
  if (backend != nullptr) {
    // Called with no engine lock held: a backend shutdown may block on I/O.
    backend->shutdown();
    report.backend_shutdown = true;
  }
  if (report.explanation.empty()) {
    report.explanation =
        "work intake stopped; in-flight attempts were abandoned as UNKNOWN and every effect claim "
        "now requires revalidation";
  }
  return report;
}


Status RateGovernor::demote_for_recovery(const ActorContext& actor, u64& attempts_marked,
                                         u64& envelopes_demoted, u64& effects_demoted) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return Status(make_error(ErrorCode::Fenced, fence, "actor incarnation is not current"));
    }
    const TimestampNs now = clock_->NowNs();
    std::vector<EnforcementAttempt> attempt_updates;
    std::vector<RateEnvelope> envelope_updates;

    for (auto& entry : attempts_) {
      EnforcementAttempt& attempt = entry.second;
      if (!attempt_is_inflight(attempt.state)) {
        continue;
      }
      EnforcementAttempt updated = attempt;
      updated.state = AttemptState::Ambiguous;
      updated.reason = ReasonCode::EnvelopeRecoveredRequiresRevalidation;
      updated.completed_ns = now;
      updated.effect_confirmed = false;
      updated.verified = false;
      updated.detail =
          "unfinished at the previous incarnation; the outcome is UNKNOWN and can never publish "
          "success";
      merge_attempt(attempt_updates, updated);
      ++attempts_marked;
    }

    for (auto& entry : envelopes_) {
      RateEnvelope& envelope = entry.second;
      const bool claims_effect = envelope.effect_verified || envelope.applied_rate_ups != 0 ||
                                 envelope.applied_burst_tokens != 0;
      const bool holds_state = envelope.state == EnvelopeState::Applied ||
                               envelope.state == EnvelopeState::Dispatching ||
                               envelope.state == EnvelopeState::Degraded ||
                               envelope.requires_revalidation;
      if (!claims_effect && !holds_state) {
        continue;
      }
      RateEnvelope updated = envelope;
      if (claims_effect) {
        ++effects_demoted;
      }
      updated.effect_verified = false;
      updated.requires_revalidation = true;
      if (updated.state == EnvelopeState::Applied || updated.state == EnvelopeState::Dispatching) {
        updated.state = EnvelopeState::Degraded;
        updated.reason = ReasonCode::EnvelopeRecoveredRequiresRevalidation;
      } else if (updated.reason == ReasonCode::None) {
        updated.reason = ReasonCode::EnvelopeRecoveredRequiresRevalidation;
      }
      updated.updated_ns = now;
      merge_envelope(envelope_updates, updated);
      ++envelopes_demoted;
    }

    if (attempt_updates.empty() && envelope_updates.empty()) {
      return Status::ok();
    }
    StagedRecords records;
    for (const EnforcementAttempt& attempt : attempt_updates) {
      stage_attempt(records, attempt, JournalRecordType::AttemptTerminated);
    }
    for (const RateEnvelope& envelope : envelope_updates) {
      stage_envelope(records, envelope, JournalRecordType::EnvelopeStateChange);
    }
    const Status status = commit_many_locked(records);
    if (!status) {
      return status;
    }
    for (const EnforcementAttempt& attempt : attempt_updates) {
      attempts_[attempt.id.value()] = attempt;
    }
    for (const RateEnvelope& envelope : envelope_updates) {
      envelopes_[envelope.id.value()] = envelope;
    }
    push_event_locked(ReasonCode::EnvelopeRecoveredRequiresRevalidation, RateEnvelopeId{},
                      Generation{}, EnforcementAttemptId{},
                      "recovery demoted " + std::to_string(attempts_marked) +
                          " unfinished attempt(s) and " + std::to_string(envelopes_demoted) +
                          " envelope(s); no durable claim was restored as current",
                      events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

void RateGovernor::collect_session_lost_updates_locked(std::vector<RateEnvelope>& out) const {
  for (const auto& entry : envelopes_) {
    const RateEnvelope& envelope = entry.second;
    const bool claims_effect = envelope.effect_verified || envelope.applied_rate_ups != 0 ||
                               envelope.state == EnvelopeState::Applied ||
                               envelope.state == EnvelopeState::Dispatching;
    if (!claims_effect) {
      continue;
    }
    RateEnvelope updated = envelope;
    updated.effect_verified = false;
    updated.requires_revalidation = true;
    if (updated.state == EnvelopeState::Applied ||
        updated.state == EnvelopeState::Dispatching) {
      updated.state = EnvelopeState::Degraded;
      updated.reason = ReasonCode::BackendSessionLost;
    } else if (updated.reason == ReasonCode::None) {
      updated.reason = ReasonCode::BackendSessionLost;
    }
    merge_envelope(out, updated);
  }
}

Status RateGovernor::mark_session_lost(const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return Status(make_error(ErrorCode::Fenced, fence, "actor incarnation is not current"));
    }
    backend_bound_ = false;
    std::vector<RateEnvelope> updates;
    collect_session_lost_updates_locked(updates);
    if (!updates.empty()) {
      StagedRecords records;
      for (const RateEnvelope& envelope : updates) {
        stage_envelope(records, envelope, JournalRecordType::EnvelopeStateChange);
      }
      const Status status = commit_many_locked(records);
      if (!status) {
        return status;
      }
      for (const RateEnvelope& envelope : updates) {
        envelopes_[envelope.id.value()] = envelope;
        ++counters_.stale_transitions;
      }
    }
    push_event_locked(ReasonCode::BackendSessionLost, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{},
                      "enforcement session lost; " + std::to_string(updates.size()) +
                          " envelope(s) now require revalidation",
                      events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------
EnvelopeAction RateGovernor::derive_action_locked(const RateEnvelope& envelope, TimestampNs now,
                                                  ReasonCode& action_reason) const {
  action_reason = ReasonCode::None;
  const ReasonCode authority = authority_status_locked(envelope, now);
  switch (envelope.state) {
    case EnvelopeState::Unknown:
      return EnvelopeAction::Halt;
    case EnvelopeState::Desired:
      return EnvelopeAction::AwaitAuthorization;
    case EnvelopeState::Authorized:
      if (authority != ReasonCode::None) {
        action_reason = authority;
        return EnvelopeAction::RejectStale;
      }
      return EnvelopeAction::RetryApply;
    case EnvelopeState::Dispatching:
      return EnvelopeAction::AwaitVerification;
    case EnvelopeState::Applied:
      if (authority != ReasonCode::None) {
        action_reason = authority;
        return EnvelopeAction::RejectStale;
      }
      if (envelope.requires_revalidation) {
        return EnvelopeAction::Revalidate;
      }
      if (envelope.revalidate_by_ns != 0 && now >= envelope.revalidate_by_ns) {
        action_reason = ReasonCode::EnvelopeRevalidationUnknown;
        return EnvelopeAction::Revalidate;
      }
      return EnvelopeAction::None;
    case EnvelopeState::Degraded:
      if (authority != ReasonCode::None) {
        action_reason = authority;
        return EnvelopeAction::Revoke;
      }
      if (envelope.requires_revalidation) {
        return EnvelopeAction::Revalidate;
      }
      if (envelope.cooldown_until_ns != 0 && now < envelope.cooldown_until_ns) {
        action_reason = ReasonCode::EnvelopeCooldownActive;
        return EnvelopeAction::None;
      }
      if (envelope.has_plan && envelope.applied_rate_ups > envelope.plan.ceiling_ups()) {
        action_reason = ReasonCode::ApplyOverDeliveredRevoking;
        return EnvelopeAction::Reduce;
      }
      return EnvelopeAction::RetryApply;
    case EnvelopeState::RevokePending:
      return EnvelopeAction::Revoke;
    case EnvelopeState::Revoked:
      return EnvelopeAction::None;
    case EnvelopeState::Stale:
      if (envelope.effect_verified || envelope.applied_rate_ups != 0) {
        return EnvelopeAction::Revoke;
      }
      return EnvelopeAction::RejectStale;
    case EnvelopeState::Failed:
      return EnvelopeAction::Halt;
  }
  return EnvelopeAction::None;
}

EnvelopeView RateGovernor::make_view_locked(const RateEnvelope& envelope, TimestampNs now) const {
  EnvelopeView view;
  view.id = envelope.id;
  view.generation = envelope.generation;
  view.state = envelope.state;
  view.reason = envelope.reason;
  view.annotation = envelope.annotation;
  view.has_plan = envelope.has_plan;
  if (envelope.has_plan) {
    view.effective_ceiling_ups = envelope.plan.ceiling_ups();
    view.effective_floor_ups = envelope.plan.floor_ups();
    view.effective_target_ups = envelope.plan.target_ups();
    view.effective_burst_tokens = envelope.plan.burst_tokens();
    view.limiting_authority = envelope.plan.limiting_authority;
    view.plan_valid_from_ns = envelope.plan.fingerprint.valid_from_ns;
    view.plan_valid_until_ns = envelope.plan.fingerprint.valid_until_ns;
  }
  view.burst_tokens_available = envelope.bucket.tokens;
  view.burst_capacity = envelope.bucket.capacity;
  view.applied_rate_ups = envelope.applied_rate_ups;
  view.applied_burst_tokens = envelope.applied_burst_tokens;
  view.effect_verified = envelope.effect_verified;
  view.applied_epoch = envelope.applied_epoch;
  view.applied_boot = envelope.applied_boot;
  view.cooldown_until_ns = envelope.cooldown_until_ns;
  view.revalidate_by_ns = envelope.revalidate_by_ns;
  view.requires_revalidation = envelope.requires_revalidation;
  view.attempts_issued = envelope.attempts_issued;
  view.completions_applied = envelope.completions_applied;
  view.completions_rejected = envelope.completions_rejected;

  const ReasonCode authority = authority_status_locked(envelope, now);
  const bool epoch_current = !envelope.applied_epoch.valid() || envelope.applied_epoch == epoch_;
  const bool effect_current =
      envelope.effect_verified && epoch_current && !envelope.requires_revalidation;
  view.legally_enforceable_now = envelope.state == EnvelopeState::Applied &&
                                 authority == ReasonCode::None && effect_current;
  view.action = derive_action_locked(envelope, now, view.action_reason);

  std::string explanation = "state=";
  explanation.append(to_string_view(envelope.state));
  explanation.append(" reason=");
  explanation.append(to_string_view(envelope.reason));
  if (envelope.has_plan) {
    explanation.append(" ceiling=");
    explanation.append(std::to_string(view.effective_ceiling_ups));
    explanation.append(" target=");
    explanation.append(std::to_string(view.effective_target_ups));
    explanation.append(" floor=");
    explanation.append(std::to_string(view.effective_floor_ups));
    explanation.append(" burst=");
    explanation.append(std::to_string(view.effective_burst_tokens));
  }
  explanation.append(" burst_remaining=");
  explanation.append(std::to_string(view.burst_tokens_available));
  explanation.append(" applied=");
  explanation.append(std::to_string(view.applied_rate_ups));
  explanation.append(view.effect_verified ? " (verified)" : " (not verified)");
  explanation.append(" enforceable_now=");
  explanation.append(view.legally_enforceable_now ? "yes" : "no");
  explanation.append(" action=");
  explanation.append(to_string_view(view.action));
  if (view.action_reason != ReasonCode::None) {
    explanation.append(" action_reason=");
    explanation.append(to_string_view(view.action_reason));
  }
  if (authority != ReasonCode::None) {
    explanation.append(" authority=");
    explanation.append(to_string_view(authority));
  }
  view.explanation = truncate_bounded(std::move(explanation), config_.max_attribute_bytes);
  return view;
}

std::string describe(const EnvelopeView& view) {
  std::string out = "envelope ";
  out.append(view.id.to_string());
  out.append("/");
  out.append(view.generation.to_string());
  out.append(" ");
  out.append(view.explanation);
  return out;
}

Result<EnvelopeView> RateGovernor::inspect(RateEnvelopeId id) const {
  ScopedLock guard(mutex_);
  const RateEnvelope* envelope = find_envelope_locked(id);
  if (envelope == nullptr) {
    return Result<EnvelopeView>(make_error(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                           "unknown envelope"));
  }
  return make_view_locked(*envelope, clock_->NowNs());
}

std::vector<EnvelopeView> RateGovernor::list_envelopes(usize limit) const {
  std::vector<EnvelopeView> views;
  ScopedLock guard(mutex_);
  const TimestampNs now = clock_->NowNs();
  const usize bound = std::min<usize>(limit, static_cast<usize>(config_.max_envelopes));
  views.reserve(std::min<usize>(bound, envelopes_.size()));
  for (const auto& entry : envelopes_) {
    if (views.size() >= bound) {
      break;
    }
    views.push_back(make_view_locked(entry.second, now));
  }
  return views;
}

Result<EnforcementAttempt> RateGovernor::inspect_attempt(EnforcementAttemptId id) const {
  ScopedLock guard(mutex_);
  const auto found = attempts_.find(id.value());
  if (found == attempts_.end()) {
    return Result<EnforcementAttempt>(make_error(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                                 "unknown attempt"));
  }
  return found->second;
}

std::vector<EnforcementAttempt> RateGovernor::list_attempts(RateEnvelopeId id, usize limit) const {
  std::vector<EnforcementAttempt> result;
  ScopedLock guard(mutex_);
  const auto found = attempts_by_envelope_.find(id.value());
  if (found == attempts_by_envelope_.end()) {
    return result;
  }
  const usize bound = std::min<usize>(limit, found->second.size());
  result.reserve(bound);
  for (usize index = 0; index < bound; ++index) {
    const auto attempt = attempts_.find(found->second[index]);
    if (attempt != attempts_.end()) {
      result.push_back(attempt->second);
    }
  }
  return result;
}

std::vector<AuditEvent> RateGovernor::drain_events(usize limit) {
  std::vector<AuditEvent> drained;
  ScopedLock guard(mutex_);
  while (!events_.empty() && drained.size() < limit) {
    drained.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return drained;
}

EngineCounters RateGovernor::counters() const {
  ScopedLock guard(mutex_);
  return counters_;
}

EngineStorageStats RateGovernor::storage_stats() const {
  EngineStorageStats stats;
  ScopedLock guard(mutex_);
  stats.grants = static_cast<u64>(grants_.size());
  stats.reservations = static_cast<u64>(reservations_.size());
  stats.policies = static_cast<u64>(policies_.size());
  stats.resources = static_cast<u64>(resources_.size());
  stats.backends = static_cast<u64>(backends_.size());
  stats.envelopes = static_cast<u64>(envelopes_.size());
  stats.attempts = static_cast<u64>(attempts_.size());
  stats.audit_events = static_cast<u64>(events_.size());
  stats.audit_events_dropped = events_dropped_;
  stats.journal_bytes = journal_ != nullptr ? journal_->size_bytes() : 0;
  stats.recovery_orphan_records = recovery_orphan_records_;
  return stats;
}

FabricEpoch RateGovernor::epoch() const {
  ScopedLock guard(mutex_);
  return epoch_;
}

bool RateGovernor::durability_degraded() const {
  ScopedLock guard(mutex_);
  return durability_degraded_;
}

}  // namespace rate_governor
