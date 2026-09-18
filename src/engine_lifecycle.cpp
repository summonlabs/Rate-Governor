#include "rate_governor/engine.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "rate_governor/codec.hpp"
#include "rate_governor/plan.hpp"
#include "record_codec.hpp"
#include "engine_lock.hpp"

namespace rate_governor {
namespace {

Status lifecycle_error(ErrorCode code, ReasonCode reason, std::string message) {
  return Status(make_error(code, reason, std::move(message)));
}

template <class T>
Result<T> lifecycle_fail(ErrorCode code, ReasonCode reason, std::string message) {
  return Result<T>(make_error(code, reason, std::move(message)));
}

bool attempt_inflight(AttemptState state) {
  return state == AttemptState::Created || state == AttemptState::Dispatched ||
         state == AttemptState::Acknowledged;
}

}  // namespace

// ---------------------------------------------------------------------------
// Plan derivation
// ---------------------------------------------------------------------------
PlanInputs RateGovernor::make_plan_inputs_locked(const RateEnvelope& envelope) const {
  PlanInputs inputs;
  inputs.envelope = envelope.id;
  inputs.envelope_generation = envelope.generation;
  inputs.flow = envelope.flow;
  inputs.flow_generation = envelope.flow_generation;
  const auto flow = flows_.find(envelope.flow.value());
  inputs.flow_def = flow == flows_.end() ? nullptr : &flow->second;
  inputs.resource = envelope.resource;
  inputs.resource_generation = envelope.resource_generation;
  const auto resource = resources_.find(envelope.resource.value());
  inputs.resource_def = resource == resources_.end() ? nullptr : &resource->second;
  inputs.grant = envelope.grant;
  inputs.grant_generation = envelope.grant_generation;
  const auto grant = grants_.find(envelope.grant.value());
  inputs.grant_def = grant == grants_.end() ? nullptr : &grant->second;
  inputs.has_reservation = envelope.has_reservation;
  inputs.reservation = envelope.reservation;
  inputs.reservation_generation = envelope.reservation_generation;
  const auto reservation = reservations_.find(envelope.reservation.value());
  inputs.reservation_def = reservation == reservations_.end() ? nullptr : &reservation->second;
  inputs.policy = envelope.policy;
  inputs.policy_generation = envelope.policy_generation;
  const auto policy = policies_.find(envelope.policy.value());
  inputs.policy_def = policy == policies_.end() ? nullptr : &policy->second;
  inputs.backend = envelope.backend;
  inputs.backend_generation = envelope.backend_generation;
  const auto backend = backends_.find(envelope.backend.value());
  inputs.backend_def = backend == backends_.end() ? nullptr : &backend->second;
  inputs.epoch = epoch_;
  inputs.boot = config_.boot;
  return inputs;
}

PlanOutcome RateGovernor::derive_plan_locked(const RateEnvelope& envelope, TimestampNs now) const {
  return derive_plan(make_plan_inputs_locked(envelope), now);
}

ReasonCode RateGovernor::authority_status_locked(const RateEnvelope& envelope,
                                                 TimestampNs now) const {
  const PlanInputs inputs = make_plan_inputs_locked(envelope);
  if (inputs.flow_def == nullptr || inputs.flow_def->generation != envelope.flow_generation ||
      !inputs.flow_def->active()) {
    return ReasonCode::EnvelopeStaleFlowGenerationChanged;
  }
  if (inputs.resource_def == nullptr ||
      inputs.resource_def->generation != envelope.resource_generation) {
    return ReasonCode::EnvelopeStaleResourceGenerationChanged;
  }
  if (!inputs.resource_def->active()) {
    return ReasonCode::EnvelopeStaleResourceGenerationChanged;
  }
  if (inputs.grant_def == nullptr ||
      inputs.grant_def->generation != envelope.grant_generation) {
    return ReasonCode::EnvelopeStaleGrantRecalled;
  }
  if (inputs.grant_def->state == GrantState::Recalled) {
    return ReasonCode::EnvelopeStaleGrantRecalled;
  }
  if (!inputs.grant_def->active()) {
    return ReasonCode::EnvelopeStaleGrantRecalled;
  }
  if (!inputs.grant_def->window.contains(now)) {
    return ReasonCode::EnvelopeStaleGrantExpired;
  }
  if (inputs.policy_def == nullptr ||
      inputs.policy_def->generation != envelope.policy_generation) {
    return ReasonCode::EnvelopeStalePolicyGenerationChanged;
  }
  if (!inputs.policy_def->active()) {
    return ReasonCode::EnvelopeStalePolicyGenerationChanged;
  }
  if (!inputs.policy_def->window.contains(now)) {
    return ReasonCode::EnvelopeStalePolicyExpired;
  }
  if (envelope.has_reservation) {
    if (inputs.reservation_def == nullptr ||
        inputs.reservation_def->generation != envelope.reservation_generation) {
      return ReasonCode::EnvelopeStaleReservationExpired;
    }
    if (!inputs.reservation_def->active() || !inputs.reservation_def->window.contains(now)) {
      return ReasonCode::EnvelopeStaleReservationExpired;
    }
  }
  if (inputs.backend_def == nullptr ||
      inputs.backend_def->generation != envelope.backend_generation) {
    return ReasonCode::EnvelopeStaleNoLiveAuthority;
  }
  if (envelope.has_plan && inputs.resource_def->capacity_ups < envelope.plan.ceiling_ups()) {
    return ReasonCode::EnvelopeStaleResourceCapacityReduced;
  }
  return ReasonCode::None;
}

// ---------------------------------------------------------------------------
// Envelope lifecycle
// ---------------------------------------------------------------------------
Result<RateEnvelopeId> RateGovernor::open_envelope(const EnvelopeRequest& request,
                                                   const ActorContext& actor) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  Result<RateEnvelopeId> result = [&]() -> Result<RateEnvelopeId> {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::ShuttingDown,
                                            ReasonCode::WorkRejectedDuringShutdown,
                                            "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::Fenced, fence,
                                            "actor incarnation is not current");
    }
    if (!request.flow.valid() || !request.resource.valid() || !request.grant.valid() ||
        !request.policy.valid() || !request.backend.valid()) {
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::InvalidArgument,
                                            ReasonCode::MalformedInput,
                                            "envelope binding is incomplete");
    }
    if (request.has_reservation && !request.reservation.valid()) {
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::InvalidArgument,
                                            ReasonCode::MalformedInput,
                                            "envelope requests a reservation without an identity");
    }
    if (!is_bounded_text(request.annotation, config_.max_attribute_bytes)) {
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::InvalidArgument, ReasonCode::MalformedInput,
                                            "annotation is not bounded printable text");
    }
    if (envelopes_.size() >= config_.max_envelopes) {
      return lifecycle_fail<RateEnvelopeId>(ErrorCode::LimitExceeded,
                                            ReasonCode::CapacityLimitExceeded,
                                            "envelope table is full");
    }

    RateEnvelope envelope;
    envelope.id = RateEnvelopeId(next_envelope_id_++);
    envelope.generation = Generation(1);
    envelope.flow = request.flow;
    envelope.resource = request.resource;
    envelope.grant = request.grant;
    envelope.policy = request.policy;
    envelope.backend = request.backend;
    envelope.has_reservation = request.has_reservation;
    envelope.reservation = request.reservation;

    // Generations of zero mean "resolve to whatever is current now"; the
    // resolved generation is recorded explicitly and never left implicit.
    auto resolve = [](Generation requested, const auto& table, u64 raw_id) -> Generation {
      if (requested.valid()) {
        return requested;
      }
      const auto found = table.find(raw_id);
      return found == table.end() ? Generation{} : found->second.generation;
    };
    envelope.flow_generation = resolve(request.flow_generation, flows_, request.flow.value());
    envelope.resource_generation =
        resolve(request.resource_generation, resources_, request.resource.value());
    envelope.grant_generation =
        resolve(request.grant_generation, grants_, request.grant.value());
    envelope.reservation_generation =
        resolve(request.reservation_generation, reservations_, request.reservation.value());
    envelope.policy_generation =
        resolve(request.policy_generation, policies_, request.policy.value());
    envelope.backend_generation =
        resolve(request.backend_generation, backends_, request.backend.value());

    envelope.state = EnvelopeState::Desired;
    envelope.reason = ReasonCode::EnvelopeOpened;
    envelope.annotation = request.annotation;
    envelope.created_ns = now;
    envelope.updated_ns = now;

    ByteWriter writer(1024);
    encode_envelope(writer, envelope);
    const Status status = commit_one_locked(JournalRecordType::EnvelopeOpened, writer.data());
    if (!status) {
      return lifecycle_fail<RateEnvelopeId>(status.code(), status.reason(), status.message());
    }
    const RateEnvelopeId id = envelope.id;
    envelopes_[id.value()] = envelope;
    ++counters_.envelopes_opened;
    push_event_locked(ReasonCode::EnvelopeOpened, id, envelope.generation, EnforcementAttemptId{},
                      "envelope opened: flow=" + envelope.flow.to_string() + "/" +
                          envelope.flow_generation.to_string() + " resource=" +
                          envelope.resource.to_string() + "/" +
                          envelope.resource_generation.to_string() + " grant=" +
                          envelope.grant.to_string() + "/" +
                          envelope.grant_generation.to_string() + " policy=" +
                          envelope.policy.to_string() + "/" +
                          envelope.policy_generation.to_string() + " backend=" +
                          envelope.backend.to_string() + "/" +
                          envelope.backend_generation.to_string(),
                      events);
    return id;
  }();
  emit(events);
  return result;
}

Result<PlanOutcome> RateGovernor::authorize(RateEnvelopeId id, const ActorContext& actor) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  PlanOutcome outcome;
  Result<PlanOutcome> result = [&]() -> Result<PlanOutcome> {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return lifecycle_fail<PlanOutcome>(ErrorCode::ShuttingDown,
                                         ReasonCode::WorkRejectedDuringShutdown,
                                         "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<PlanOutcome>(ErrorCode::Fenced, fence,
                                         "actor incarnation is not current");
    }
    RateEnvelope* envelope = find_envelope_locked(id);
    if (envelope == nullptr) {
      return lifecycle_fail<PlanOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                         "unknown envelope");
    }
    outcome = derive_plan_locked(*envelope, now);
    RateEnvelope updated = *envelope;
    StagedRecords records;

    if (!outcome.authorized) {
      ++counters_.plans_denied;
      updated.reason = outcome.reason;
      updated.updated_ns = now;
      if (updated.state == EnvelopeState::Desired || updated.state == EnvelopeState::Authorized) {
        updated.state = EnvelopeState::Failed;
      }
      stage_envelope(records, updated, JournalRecordType::EnvelopeStateChange);
      const Status status = commit_records_locked(records);
      if (!status) {
        return lifecycle_fail<PlanOutcome>(status.code(), status.reason(), status.message());
      }
      *envelope = updated;
      push_event_locked(outcome.reason, id, updated.generation, EnforcementAttemptId{},
                        outcome.explanation, events);
      return outcome;
    }

    ++counters_.plans_authorized;
    const bool binding_unchanged =
        envelope->has_plan && envelope->plan.fingerprint.same_binding(outcome.plan.fingerprint) &&
        envelope->plan.fingerprint.effective_ceiling_ups ==
            outcome.plan.fingerprint.effective_ceiling_ups &&
        envelope->plan.fingerprint.effective_burst_tokens ==
            outcome.plan.fingerprint.effective_burst_tokens;
    updated.has_plan = true;
    updated.plan = outcome.plan;
    ++updated.plan_revision;
    updated.updated_ns = now;
    if (envelope->state == EnvelopeState::Applied || envelope->state == EnvelopeState::Degraded) {
      if (!binding_unchanged) {
        updated.state = EnvelopeState::Degraded;
        updated.reason = ReasonCode::EnvelopeReducedByPolicy;
        updated.requires_revalidation = false;
      }
    } else if (envelope->state != EnvelopeState::RevokePending &&
               envelope->state != EnvelopeState::Revoked) {
      updated.state = EnvelopeState::Authorized;
      updated.reason = ReasonCode::EnvelopeAuthorized;
    }
    if (!updated.bucket.initialized || updated.bucket.capacity != outcome.plan.burst_tokens() ||
        updated.bucket.rate_ups != outcome.plan.ceiling_ups()) {
      configure_bucket(updated.bucket, outcome.plan.burst_tokens(), outcome.plan.ceiling_ups(), now);
    }
    stage_envelope(records, updated, JournalRecordType::EnvelopeAuthorized);
    const Status status = commit_records_locked(records);
    if (!status) {
      return lifecycle_fail<PlanOutcome>(status.code(), status.reason(), status.message());
    }
    *envelope = updated;
    push_event_locked(outcome.reason, id, updated.generation, EnforcementAttemptId{},
                      outcome.explanation, events);
    return outcome;
  }();
  emit(events);
  return result;
}

Result<PlanOutcome> RateGovernor::explain_plan(RateEnvelopeId id) const {
  ScopedLock guard(mutex_);
  const RateEnvelope* envelope = find_envelope_locked(id);
  if (envelope == nullptr) {
    return lifecycle_fail<PlanOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                       "unknown envelope");
  }
  return derive_plan_locked(*envelope, clock_->NowNs());
}

ApplyRequest RateGovernor::make_apply_request(const RateEnvelope& envelope,
                                              const EnforcementAttempt& attempt) const {
  ApplyRequest request;
  request.attempt = attempt.id;
  request.envelope = envelope.id;
  request.envelope_generation = envelope.generation;
  request.fingerprint = attempt.fingerprint;
  request.rate_ups = envelope.plan.ceiling_ups();
  request.burst_tokens = envelope.plan.burst_tokens();
  request.refill = envelope.plan.refill;
  request.epoch = attempt.epoch;
  request.boot = attempt.boot;
  request.issued_ns = attempt.dispatched_ns;
  return request;
}

RevokeRequest RateGovernor::make_revoke_request(const RateEnvelope& envelope,
                                                const EnforcementAttempt& attempt,
                                                ReasonCode reason) const {
  RevokeRequest request;
  request.attempt = attempt.id;
  request.envelope = envelope.id;
  request.envelope_generation = envelope.generation;
  request.fingerprint = attempt.fingerprint;
  request.epoch = attempt.epoch;
  request.boot = attempt.boot;
  request.reason = reason;
  request.issued_ns = attempt.dispatched_ns;
  return request;
}

Result<ApplyDispatch> RateGovernor::apply(RateEnvelopeId id, const ActorContext& actor) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  ApplyRequest request;
  EnforcementAttempt attempt_snapshot;
  IEnforcementBackend* backend = nullptr;
  bool proceed = false;
  bool session_lost = false;

  Result<ApplyDispatch> phase_one = [&]() -> Result<ApplyDispatch> {
    ScopedLock guard(mutex_);
    ApplyDispatch dispatch;
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return lifecycle_fail<ApplyDispatch>(ErrorCode::ShuttingDown,
                                           ReasonCode::WorkRejectedDuringShutdown,
                                           "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<ApplyDispatch>(ErrorCode::Fenced, fence,
                                           "actor incarnation is not current");
    }
    RateEnvelope* envelope = find_envelope_locked(id);
    if (envelope == nullptr) {
      return lifecycle_fail<ApplyDispatch>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                           "unknown envelope");
    }
    if (envelope->state == EnvelopeState::Revoked) {
      return lifecycle_fail<ApplyDispatch>(ErrorCode::Conflict, ReasonCode::EnvelopeRevoked,
                                           "envelope is revoked; open a new binding");
    }
    if (envelope->cooldown_until_ns != 0 && now < envelope->cooldown_until_ns) {
      dispatch.attempt = EnforcementAttemptId{};
      return lifecycle_fail<ApplyDispatch>(ErrorCode::Denied, ReasonCode::EnvelopeCooldownActive,
                                           "envelope is in cooldown after a recent attempt");
    }
    const u64 key = compute_idempotency_key(envelope->id, envelope->generation, AttemptKind::Apply);
    for (const auto& entry : attempts_) {
      if (entry.second.idempotency_key == key && attempt_inflight(entry.second.state)) {
        ++counters_.idempotent_replays;
        dispatch.attempt = entry.second.id;
        dispatch.duplicate = true;
        dispatch.reason = ReasonCode::AttemptIdempotentReplay;
        dispatch.resulting_state = envelope->state;
        dispatch.explanation =
            "an equivalent apply attempt is already in flight; no second dispatch was issued";
        return dispatch;
      }
    }
    if (backend_ == nullptr || !backend_bound_) {
      return lifecycle_fail<ApplyDispatch>(ErrorCode::BackendFailure,
                                           ReasonCode::BackendSessionLost,
                                           "no backend session admitted at the current fabric epoch");
    }
    if (!backend_->session_live()) {
      session_lost = true;
      return lifecycle_fail<ApplyDispatch>(ErrorCode::BackendFailure,
                                           ReasonCode::BackendSessionLost,
                                           "the enforcement session is no longer live");
    }
    if (backend_->describe().id != envelope->backend ||
        backend_->describe().generation != envelope->backend_generation) {
      return lifecycle_fail<ApplyDispatch>(ErrorCode::StaleAuthority,
                                           ReasonCode::AttemptFencedByGeneration,
                                           "bound backend identity does not match the envelope");
    }

    const PlanOutcome plan = derive_plan_locked(*envelope, now);
    if (!plan.authorized) {
      ++counters_.plans_denied;
      RateEnvelope denied = *envelope;
      denied.reason = plan.reason;
      denied.updated_ns = now;
      StagedRecords records;
      stage_envelope(records, denied, JournalRecordType::EnvelopeStateChange);
      const Status status = commit_records_locked(records);
      if (!status) {
        return lifecycle_fail<ApplyDispatch>(status.code(), status.reason(), status.message());
      }
      *envelope = denied;
      push_event_locked(plan.reason, id, denied.generation, EnforcementAttemptId{},
                        plan.explanation, events);
      return lifecycle_fail<ApplyDispatch>(ErrorCode::Denied, plan.reason, plan.explanation);
    }

    RateEnvelope updated = *envelope;
    StagedRecords records;
    const bool plan_changed =
        !updated.has_plan ||
        !updated.plan.fingerprint.same_binding(plan.plan.fingerprint) ||
        updated.plan.fingerprint.effective_ceiling_ups !=
            plan.plan.fingerprint.effective_ceiling_ups ||
        updated.plan.fingerprint.effective_burst_tokens !=
            plan.plan.fingerprint.effective_burst_tokens;
    if (plan_changed) {
      updated.has_plan = true;
      updated.plan = plan.plan;
      ++updated.plan_revision;
      stage_envelope(records, updated, JournalRecordType::EnvelopeAuthorized);
    }

    EnforcementAttempt attempt;
    attempt.id = EnforcementAttemptId(next_attempt_id_++);
    attempt.envelope = updated.id;
    attempt.envelope_generation = updated.generation;
    attempt.kind = AttemptKind::Apply;
    attempt.state = AttemptState::Created;
    attempt.reason = ReasonCode::ApplyDispatched;
    attempt.fingerprint = updated.plan.fingerprint;
    attempt.backend = updated.backend;
    attempt.backend_generation = updated.backend_generation;
    attempt.epoch = epoch_;
    const WorkerBootId session_boot = backend_->session_boot();
    attempt.boot = session_boot.valid() ? session_boot : config_.boot;
    attempt.sequence = ++attempt_sequence_;
    attempt.idempotency_key = key;
    attempt.created_ns = now;
    attempt.dispatched_ns = now;
    attempt.deadline_ns = config_.attempt_deadline_ns == 0
                              ? 0
                              : saturating_add(now, config_.attempt_deadline_ns);
    attempt.requested_rate_ups = updated.plan.ceiling_ups();
    attempt.requested_burst_tokens = updated.plan.burst_tokens();
    attempt.detail = "apply dispatched";

    updated.state = EnvelopeState::Dispatching;
    updated.reason = ReasonCode::ApplyDispatched;
    updated.updated_ns = now;
    ++updated.attempts_issued;
    configure_bucket(updated.bucket, updated.plan.burst_tokens(), updated.plan.ceiling_ups(), now);

    stage_attempt(records, attempt, JournalRecordType::AttemptCreated);
    stage_envelope(records, updated, JournalRecordType::EnvelopeStateChange);
    const Status status = commit_records_locked(records);
    if (!status) {
      return lifecycle_fail<ApplyDispatch>(status.code(), status.reason(), status.message());
    }
    *envelope = updated;
    (void)retain_attempt_locked(attempt);
    ++counters_.applies_dispatched;

    attempt_snapshot = attempt;
    request = make_apply_request(updated, attempt);
    backend = backend_;
    proceed = true;
    dispatch.attempt = attempt.id;
    dispatch.dispatched = true;
    dispatch.resulting_state = updated.state;
    dispatch.reason = ReasonCode::ApplyDispatched;
    dispatch.explanation = plan.explanation;
    return dispatch;
  }();

  emit(events);
  if (session_lost) {
    // Detecting the loss is itself authoritative: every claim it supported must
    // stop being presented as current.
    (void)mark_session_lost(actor);
  }
  if (!phase_one) {
    return phase_one;
  }
  if (!proceed) {
    return phase_one;
  }

  // Dispatch happens with no engine lock held: a backend call is never made
  // while authoritative state is locked.
  const DispatchOutcome dispatch = backend->dispatch_apply(request);

  CompletionReport report;
  report.status = dispatch.status;
  report.acknowledged = dispatch.status == DispatchStatus::Completed;
  report.acknowledged_rate_ups = dispatch.applied_rate_ups;
  report.acknowledged_burst_tokens = dispatch.applied_burst_tokens;
  report.epoch = dispatch.epoch;
  report.boot = dispatch.boot;
  report.reason = dispatch.reason;
  report.detail = dispatch.detail;
  report.received_ns = clock_->NowNs();

  Result<CompletionOutcome> completion = finish_apply(id, attempt_snapshot.id, request, report);
  if (!completion) {
    return lifecycle_fail<ApplyDispatch>(completion.code(), completion.reason(),
                                         completion.message());
  }
  ApplyDispatch result = phase_one.value();
  result.verification_performed = completion.value().verification_performed;
  result.verified = completion.value().verified;
  result.compensating_revoke_required = completion.value().compensating_revoke_required;
  result.observed_rate_ups = dispatch.applied_rate_ups;
  result.resulting_state = completion.value().resulting_state;
  result.reason = completion.value().reason;
  result.awaiting_completion = completion.value().reason == ReasonCode::ApplyDispatched;
  result.explanation = completion.value().explanation;
  return result;
}

Result<CompletionOutcome> RateGovernor::finish_apply(RateEnvelopeId envelope_id,
                                                     EnforcementAttemptId attempt_id,
                                                     const ApplyRequest& request,
                                                     const CompletionReport& report) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  CompletionOutcome outcome;
  outcome.attempt = attempt_id;
  bool need_verify = false;
  IEnforcementBackend* backend = nullptr;

  auto reject_late = [&](RateEnvelope& envelope, EnforcementAttempt& attempt, ReasonCode reason,
                         bool reports_effect, std::string detail) -> CompletionOutcome {
    CompletionOutcome rejected;
    rejected.attempt = attempt.id;
    rejected.rejected_late = true;
    rejected.reason = reason;
    EnforcementAttempt updated_attempt = attempt;
    if (updated_attempt.state != AttemptState::Ambiguous) {
      updated_attempt.state = AttemptState::LateRejected;
    }
    updated_attempt.reason = reason;
    updated_attempt.completed_ns = now;
    updated_attempt.compensating = reports_effect;
    updated_attempt.detail = truncate_bounded(std::move(detail), config_.max_attribute_bytes);
    RateEnvelope updated_envelope = envelope;
    ++updated_envelope.completions_rejected;
    updated_envelope.updated_ns = now;
    if (reports_effect) {
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      if (updated_envelope.state != EnvelopeState::Revoked) {
        updated_envelope.state = EnvelopeState::RevokePending;
        // The envelope keeps the reason that invalidated it: the compensating
        // revoke is an action, not a cause.
        updated_envelope.reason =
            reason == ReasonCode::None ? ReasonCode::CompensatingRevokeIssued : reason;
      }
      rejected.compensating_revoke_required = true;
    }
    StagedRecords records;
    stage_attempt(records, updated_attempt, JournalRecordType::AttemptTerminated);
    stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeStateChange);
    const Status status = commit_records_locked(records);
    if (!status) {
      rejected.explanation = std::string("late completion could not be recorded durably: ") +
                             status.message();
      rejected.resulting_state = envelope.state;
      return rejected;
    }
    attempt = updated_attempt;
    envelope = updated_envelope;
    ++counters_.completions_rejected;
    ++counters_.stale_transitions;
    rejected.resulting_state = envelope.state;
    rejected.explanation = std::string("completion rejected (") +
                           std::string(to_string_view(reason)) +
                           "); no authoritative effect was published";
    push_event_locked(reason, envelope.id, envelope.generation, attempt.id, rejected.explanation,
                      events);
    return rejected;
  };

  {
    ScopedLock guard(mutex_);
    EnforcementAttempt* attempt = find_attempt_locked(attempt_id);
    RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (attempt == nullptr || envelope == nullptr) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt or envelope");
    }
    outcome.resulting_state = envelope->state;
    if (attempt->kind != AttemptKind::Apply) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::InvalidArgument,
                                               ReasonCode::MalformedInput,
                                               "attempt is not an apply attempt");
    }
    const bool fencing_boot =
        report.boot.valid() && attempt->boot.valid() && report.boot != attempt->boot;
    const bool fencing_epoch = attempt->fingerprint.epoch != epoch_ ||
                               (report.epoch.valid() && report.epoch != epoch_);
    if (attempt->state == AttemptState::Verified ||
        attempt->state == AttemptState::DuplicateIgnored) {
      ++counters_.completions_duplicate;
      outcome.duplicate = true;
      outcome.reason = ReasonCode::AttemptDuplicateIgnored;
      outcome.resulting_state = envelope->state;
      outcome.explanation = "completion was already applied; the duplicate changed nothing";
      emit(events);
      return outcome;
    }
    if (!attempt_inflight(attempt->state) || fencing_boot || fencing_epoch) {
      const ReasonCode reason = fencing_boot  ? ReasonCode::AttemptFencedByBoot
                                : fencing_epoch ? ReasonCode::AttemptFencedByEpoch
                                                : ReasonCode::AttemptLateCompletionRejected;
      CompletionOutcome rejected =
          reject_late(*envelope, *attempt, reason, report.acknowledged,
                      fencing_boot  ? "completion carried a foreign boot id"
                      : fencing_epoch ? "completion carried a superseded fabric epoch"
                                      : "attempt was no longer in flight");
      emit(events);
      return rejected;
    }
    const ReasonCode stale = authority_status_locked(*envelope, now);
    if (stale != ReasonCode::None) {
      CompletionOutcome rejected =
          reject_late(*envelope, *attempt, stale, report.acknowledged,
                      "the authority that funded this attempt changed while it was in flight");
      emit(events);
      return rejected;
    }

    if (report.status == DispatchStatus::Pending) {
      EnforcementAttempt updated = *attempt;
      updated.state = AttemptState::Dispatched;
      updated.reason = ReasonCode::ApplyDispatched;
      updated.detail = "backend accepted the work and will complete asynchronously";
      StagedRecords records;
      stage_attempt(records, updated, JournalRecordType::AttemptDispatched);
      const Status status = commit_records_locked(records);
      if (!status) {
        emit(events);
        return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
      }
      *attempt = updated;
      outcome.accepted = true;
      outcome.reason = ReasonCode::ApplyDispatched;
      outcome.resulting_state = envelope->state;
      outcome.explanation = "awaiting an asynchronous completion; nothing is authoritative yet";
      emit(events);
      return outcome;
    }

    if (!report.acknowledged) {
      const ReasonCode reason =
          report.reason == ReasonCode::None ? ReasonCode::ApplyBackendFailed : report.reason;
      EnforcementAttempt updated = *attempt;
      updated.state = report.status == DispatchStatus::Failed ? AttemptState::Ambiguous
                                                              : AttemptState::Failed;
      updated.reason = reason;
      updated.completed_ns = now;
      updated.detail = report.detail.empty() ? std::string("backend refused the apply")
                                             : report.detail;
      RateEnvelope updated_envelope = *envelope;
      updated_envelope.state = EnvelopeState::Degraded;
      updated_envelope.reason = reason;
      updated_envelope.updated_ns = now;
      ++updated_envelope.completions_rejected;
      updated_envelope.requires_revalidation = updated.state == AttemptState::Ambiguous;
      updated_envelope.effect_verified = false;
      if (envelope->has_plan && envelope->plan.hysteresis.cooldown_ns != 0) {
        updated_envelope.cooldown_until_ns =
            saturating_add(now, envelope->plan.hysteresis.cooldown_ns);
      }
      StagedRecords records;
      stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
      stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeStateChange);
      const Status status = commit_records_locked(records);
      if (!status) {
        emit(events);
        return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
      }
      *attempt = updated;
      *envelope = updated_envelope;
      ++counters_.applies_degraded;
      push_event_locked(reason, envelope_id, updated_envelope.generation, attempt_id,
                        updated.detail, events);
      outcome.accepted = true;
      outcome.reason = reason;
      outcome.resulting_state = updated_envelope.state;
      outcome.explanation = "the backend refused the apply; nothing is enforced";
      emit(events);
      return outcome;
    }

    EnforcementAttempt acknowledged = *attempt;
    acknowledged.acknowledged = true;
    acknowledged.acknowledged_rate_ups = report.acknowledged_rate_ups;
    acknowledged.acknowledged_burst_tokens = report.acknowledged_burst_tokens;
    acknowledged.state = AttemptState::Acknowledged;
    acknowledged.reason = ReasonCode::ApplyAcknowledged;
    acknowledged.completed_ns = now;
    acknowledged.detail = report.detail.empty() ? std::string("acknowledged") : report.detail;
    StagedRecords records;
    stage_attempt(records, acknowledged, JournalRecordType::AttemptAcknowledged);
    const Status status = commit_records_locked(records);
    if (!status) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
    }
    *attempt = acknowledged;
    backend = backend_;
    need_verify = true;
    outcome.reason = ReasonCode::ApplyAcknowledged;
    outcome.explanation = "acknowledgement recorded; the effect is not authoritative until it is "
                          "read back and confirmed";
  }
  emit(events);
  events.clear();
  if (!need_verify) {
    return outcome;
  }

  // ---- Phase B: post-apply verification with no lock held ----
  VerificationOutcome verification;
  if (backend == nullptr) {
    verification.status = VerificationStatus::Failed;
    verification.reason = ReasonCode::ApplyVerificationUnknown;
    verification.detail = "no backend session is bound; the effect is UNKNOWN";
  } else {
    verification = backend->verify(request);
  }
  outcome.verification_performed = true;

  // ---- Phase C: commit the verified outcome, or reject it as stale ----
  {
    ScopedLock guard(mutex_);
    EnforcementAttempt* attempt = find_attempt_locked(attempt_id);
    RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (attempt == nullptr || envelope == nullptr) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt or envelope");
    }
    if (attempt->state != AttemptState::Acknowledged) {
      CompletionOutcome rejected =
          reject_late(*envelope, *attempt, ReasonCode::AttemptCancelled,
                      verification.observed_present,
                      "the attempt left the in-flight state while the readback was outstanding");
      emit(events);
      return rejected;
    }
    const ReasonCode stale = authority_status_locked(*envelope, now);
    if (stale != ReasonCode::None || attempt->fingerprint.epoch != epoch_) {
      const ReasonCode reason =
          attempt->fingerprint.epoch != epoch_ ? ReasonCode::EnvelopeStaleFabricEpochAdvanced
                                               : stale;
      CompletionOutcome rejected =
          reject_late(*envelope, *attempt, reason, verification.observed_present,
                      "authority changed while the readback was outstanding");
      emit(events);
      return rejected;
    }

    EnforcementAttempt updated = *attempt;
    updated.verified_rate_ups = verification.observed_rate_ups;
    updated.verified_burst_tokens = verification.observed_burst_tokens;
    updated.completed_ns = now;
    RateEnvelope updated_envelope = *envelope;
    updated_envelope.updated_ns = now;
    ++updated_envelope.completions_applied;

    if (verification.status == VerificationStatus::Unsupported) {
      updated.state = AttemptState::Ambiguous;
      updated.reason = ReasonCode::ApplyVerificationUnsupported;
      updated_envelope.state = EnvelopeState::Degraded;
      updated_envelope.reason = ReasonCode::ApplyVerificationUnsupported;
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      ++counters_.applies_degraded;
      outcome.disputed = true;
    } else if (verification.status == VerificationStatus::Failed) {
      updated.state = AttemptState::Ambiguous;
      updated.reason = ReasonCode::ApplyVerificationUnknown;
      updated_envelope.state = EnvelopeState::Degraded;
      updated_envelope.reason = ReasonCode::ApplyVerificationUnknown;
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      ++counters_.applies_degraded;
      outcome.disputed = true;
    } else if (!verification.observed_present) {
      updated.state = AttemptState::Failed;
      updated.reason = ReasonCode::ApplyVerificationMismatch;
      updated_envelope.state = EnvelopeState::Degraded;
      updated_envelope.reason = ReasonCode::ApplyVerificationMismatch;
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      ++counters_.applies_degraded;
      outcome.disputed = true;
    } else if (verification.observed_rate_ups == request.rate_ups &&
               verification.observed_burst_tokens == request.burst_tokens) {
      updated.state = AttemptState::Verified;
      updated.verified = true;
      updated.effect_confirmed = true;
      updated.reason = ReasonCode::ApplyVerified;
      updated_envelope.state = EnvelopeState::Applied;
      updated_envelope.reason = ReasonCode::ApplyVerified;
      updated_envelope.effect_verified = true;
      updated_envelope.requires_revalidation = false;
      updated_envelope.applied_rate_ups = verification.observed_rate_ups;
      updated_envelope.applied_burst_tokens = verification.observed_burst_tokens;
      updated_envelope.applied_epoch = verification.epoch.valid() ? verification.epoch : epoch_;
      updated_envelope.applied_boot = verification.boot;
      updated_envelope.cooldown_until_ns = 0;
      if (config_.revalidate_interval_ns != 0) {
        updated_envelope.revalidate_by_ns = saturating_add(now, config_.revalidate_interval_ns);
      }
      ++counters_.applies_verified;
      outcome.verified = true;
    } else if (verification.observed_rate_ups > request.rate_ups) {
      updated.state = AttemptState::Verified;
      updated.verified = true;
      updated.compensating = true;
      updated.reason = ReasonCode::ApplyOverDeliveredRevoking;
      updated_envelope.state = EnvelopeState::RevokePending;
      updated_envelope.reason = ReasonCode::ApplyOverDeliveredRevoking;
      updated_envelope.effect_verified = false;
      updated_envelope.requires_revalidation = true;
      updated_envelope.applied_rate_ups = verification.observed_rate_ups;
      updated_envelope.applied_burst_tokens = verification.observed_burst_tokens;
      updated_envelope.applied_epoch = verification.epoch.valid() ? verification.epoch : epoch_;
      updated_envelope.applied_boot = verification.boot;
      ++counters_.applies_degraded;
      outcome.compensating_revoke_required = true;
      outcome.disputed = true;
    } else {
      const u64 floor = envelope->has_plan ? envelope->plan.floor_ups() : 0;
      updated.state = AttemptState::Verified;
      updated.verified = true;
      updated.reason = ReasonCode::ApplyUnderDelivered;
      updated_envelope.applied_rate_ups = verification.observed_rate_ups;
      updated_envelope.applied_burst_tokens = verification.observed_burst_tokens;
      updated_envelope.applied_epoch = verification.epoch.valid() ? verification.epoch : epoch_;
      updated_envelope.applied_boot = verification.boot;
      updated_envelope.effect_verified = true;
      if (verification.observed_rate_ups >= floor) {
        updated_envelope.state = EnvelopeState::Degraded;
        updated_envelope.reason = ReasonCode::ApplyUnderDelivered;
        updated_envelope.requires_revalidation = false;
      } else {
        updated.state = AttemptState::Verified;
        updated.compensating = true;
        updated_envelope.state = EnvelopeState::RevokePending;
        updated_envelope.reason = ReasonCode::ApplyUnderDelivered;
        updated_envelope.requires_revalidation = true;
        outcome.compensating_revoke_required = true;
      }
      ++counters_.applies_degraded;
      outcome.disputed = true;
    }
    updated.detail = verification.detail.empty() ? updated.detail : verification.detail;

    StagedRecords records;
    stage_attempt(records, updated, JournalRecordType::AttemptVerified);
    stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeEffectRecorded);
    const Status status = commit_records_locked(records);
    if (!status) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
    }
    *attempt = updated;
    *envelope = updated_envelope;
    outcome.accepted = true;
    outcome.reason = updated.reason;
    outcome.resulting_state = updated_envelope.state;
    outcome.explanation = std::string("effect readback: requested=") +
                          std::to_string(request.rate_ups) + " observed=" +
                          std::to_string(verification.observed_rate_ups) + " state=" +
                          std::string(to_string_view(updated_envelope.state));
    push_event_locked(updated.reason, envelope_id, updated_envelope.generation, attempt_id,
                      outcome.explanation, events);
  }
  emit(events);
  return outcome;
}

Result<CompletionOutcome> RateGovernor::complete_apply(EnforcementAttemptId attempt,
                                                       const CompletionReport& report,
                                                       const ActorContext& actor) {
  ApplyRequest request;
  RateEnvelopeId envelope_id;
  {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<CompletionOutcome>(ErrorCode::Fenced, fence,
                                               "actor incarnation is not current");
    }
    EnforcementAttempt* found = find_attempt_locked(attempt);
    if (found == nullptr) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt");
    }
    if (found->kind != AttemptKind::Apply) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::InvalidArgument,
                                               ReasonCode::MalformedInput,
                                               "attempt is not an apply attempt");
    }
    envelope_id = found->envelope;
    const RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (envelope == nullptr) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "attempt references an unknown envelope");
    }
    request = make_apply_request(*envelope, *found);
  }
  return finish_apply(envelope_id, attempt, request, report);
}

Result<RevokeDispatch> RateGovernor::revoke(RateEnvelopeId id, ReasonCode reason,
                                            const ActorContext& actor) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  RevokeRequest request;
  EnforcementAttempt attempt_snapshot;
  IEnforcementBackend* backend = nullptr;
  bool proceed = false;
  bool session_lost = false;

  Result<RevokeDispatch> phase_one = [&]() -> Result<RevokeDispatch> {
    ScopedLock guard(mutex_);
    RevokeDispatch dispatch;
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return lifecycle_fail<RevokeDispatch>(ErrorCode::ShuttingDown,
                                            ReasonCode::WorkRejectedDuringShutdown,
                                            "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<RevokeDispatch>(ErrorCode::Fenced, fence,
                                            "actor incarnation is not current");
    }
    RateEnvelope* envelope = find_envelope_locked(id);
    if (envelope == nullptr) {
      return lifecycle_fail<RevokeDispatch>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                            "unknown envelope");
    }
    if (envelope->state == EnvelopeState::Revoked) {
      ++counters_.idempotent_replays;
      dispatch.duplicate = true;
      dispatch.resulting_state = envelope->state;
      dispatch.reason = ReasonCode::EnvelopeRevoked;
      dispatch.explanation = "envelope is already revoked";
      return dispatch;
    }
    const u64 key =
        compute_idempotency_key(envelope->id, envelope->generation, AttemptKind::Revoke);
    for (const auto& entry : attempts_) {
      if (entry.second.idempotency_key == key && attempt_inflight(entry.second.state)) {
        ++counters_.idempotent_replays;
        dispatch.attempt = entry.second.id;
        dispatch.duplicate = true;
        dispatch.resulting_state = envelope->state;
        dispatch.reason = ReasonCode::AttemptIdempotentReplay;
        dispatch.explanation = "an equivalent revoke attempt is already in flight";
        return dispatch;
      }
    }
    if (backend_ == nullptr || !backend_bound_) {
      return lifecycle_fail<RevokeDispatch>(ErrorCode::BackendFailure,
                                            ReasonCode::BackendSessionLost,
                                            "no backend session admitted at the current fabric epoch");
    }
    if (!backend_->session_live()) {
      session_lost = true;
      return lifecycle_fail<RevokeDispatch>(ErrorCode::BackendFailure,
                                            ReasonCode::BackendSessionLost,
                                            "the enforcement session is no longer live");
    }

    RateEnvelope updated = *envelope;
    EnforcementAttempt attempt;
    attempt.id = EnforcementAttemptId(next_attempt_id_++);
    attempt.envelope = updated.id;
    attempt.envelope_generation = updated.generation;
    attempt.kind = AttemptKind::Revoke;
    attempt.state = AttemptState::Created;
    attempt.reason = reason == ReasonCode::None ? ReasonCode::RevokeDispatched : reason;
    attempt.fingerprint = updated.has_plan ? updated.plan.fingerprint : PlanFingerprint{};
    attempt.backend = updated.backend;
    attempt.backend_generation = updated.backend_generation;
    attempt.epoch = epoch_;
    const WorkerBootId session_boot = backend_->session_boot();
    attempt.boot = session_boot.valid() ? session_boot : config_.boot;
    attempt.sequence = ++attempt_sequence_;
    attempt.idempotency_key = key;
    attempt.created_ns = now;
    attempt.dispatched_ns = now;
    attempt.deadline_ns =
        config_.attempt_deadline_ns == 0 ? 0 : saturating_add(now, config_.attempt_deadline_ns);
    attempt.detail = "revoke dispatched";

    updated.state = EnvelopeState::RevokePending;
    updated.reason = attempt.reason;
    updated.updated_ns = now;
    ++updated.attempts_issued;

    StagedRecords records;
    stage_attempt(records, attempt, JournalRecordType::AttemptCreated);
    stage_envelope(records, updated, JournalRecordType::EnvelopeStateChange);
    const Status status = commit_records_locked(records);
    if (!status) {
      return lifecycle_fail<RevokeDispatch>(status.code(), status.reason(), status.message());
    }
    *envelope = updated;
    (void)retain_attempt_locked(attempt);
    ++counters_.revokes_dispatched;

    attempt_snapshot = attempt;
    request = make_revoke_request(updated, attempt, attempt.reason);
    backend = backend_;
    proceed = true;
    dispatch.attempt = attempt.id;
    dispatch.dispatched = true;
    dispatch.resulting_state = updated.state;
    dispatch.reason = attempt.reason;
    dispatch.explanation = "revoke dispatched; effect is not authoritative until confirmed";
    return dispatch;
  }();

  emit(events);
  if (session_lost) {
    (void)mark_session_lost(actor);
  }
  if (!phase_one || !proceed) {
    return phase_one;
  }

  const DispatchOutcome dispatch = backend->dispatch_revoke(request);
  CompletionReport report;
  report.status = dispatch.status;
  report.acknowledged = dispatch.status == DispatchStatus::Completed;
  report.epoch = dispatch.epoch;
  report.boot = dispatch.boot;
  report.reason = dispatch.reason;
  report.detail = dispatch.detail;
  report.received_ns = clock_->NowNs();
  Result<CompletionOutcome> completion = finish_revoke(id, attempt_snapshot.id, request, report);
  if (!completion) {
    return lifecycle_fail<RevokeDispatch>(completion.code(), completion.reason(),
                                          completion.message());
  }
  RevokeDispatch result = phase_one.value();
  result.verified = completion.value().verified;
  result.resulting_state = completion.value().resulting_state;
  result.reason = completion.value().reason;
  result.explanation = completion.value().explanation;
  return result;
}

Result<CompletionOutcome> RateGovernor::finish_revoke(RateEnvelopeId envelope_id,
                                                      EnforcementAttemptId attempt_id,
                                                      const RevokeRequest& request,
                                                      const CompletionReport& report) {
  (void)request;  // the readback is performed through a probe built from the attempt
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  CompletionOutcome outcome;
  outcome.attempt = attempt_id;
  bool need_verify = false;
  IEnforcementBackend* backend = nullptr;
  ApplyRequest probe;

  {
    ScopedLock guard(mutex_);
    EnforcementAttempt* attempt = find_attempt_locked(attempt_id);
    RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (attempt == nullptr || envelope == nullptr) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt or envelope");
    }
    outcome.resulting_state = envelope->state;
    if (attempt->kind != AttemptKind::Revoke) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::InvalidArgument,
                                               ReasonCode::MalformedInput,
                                               "attempt is not a revoke attempt");
    }
    if (attempt->state == AttemptState::Verified ||
        attempt->state == AttemptState::DuplicateIgnored) {
      ++counters_.completions_duplicate;
      outcome.duplicate = true;
      outcome.reason = ReasonCode::AttemptDuplicateIgnored;
      outcome.explanation = "revoke completion was already applied";
      emit(events);
      return outcome;
    }
    const bool fencing_boot =
        report.boot.valid() && attempt->boot.valid() && report.boot != attempt->boot;
    const bool fencing_epoch =
        attempt->fingerprint.epoch.valid() && attempt->fingerprint.epoch != epoch_;
    if (!attempt_inflight(attempt->state) || fencing_boot || fencing_epoch) {
      EnforcementAttempt updated = *attempt;
      updated.state = AttemptState::LateRejected;
      updated.reason = fencing_boot  ? ReasonCode::AttemptFencedByBoot
                       : fencing_epoch ? ReasonCode::AttemptFencedByEpoch
                                       : ReasonCode::AttemptLateCompletionRejected;
      updated.completed_ns = now;
      updated.detail = "revoke completion arrived after the attempt stopped being in flight";
      StagedRecords records;
      stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
      const Status status = commit_records_locked(records);
      if (!status) {
        emit(events);
        return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
      }
      *attempt = updated;
      ++counters_.completions_rejected;
      outcome.reason = updated.reason;
      outcome.rejected_late = true;
      outcome.explanation = updated.detail;
      push_event_locked(updated.reason, envelope_id, envelope->generation, attempt_id,
                        updated.detail, events);
      emit(events);
      return outcome;
    }
    if (!report.acknowledged) {
      EnforcementAttempt updated = *attempt;
      updated.state = report.status == DispatchStatus::Failed ? AttemptState::Ambiguous
                                                              : AttemptState::Failed;
      updated.reason = report.reason == ReasonCode::None ? ReasonCode::RevokeBackendFailed
                                                         : report.reason;
      updated.completed_ns = now;
      updated.detail = report.detail.empty() ? std::string("backend refused the revoke")
                                             : report.detail;
      RateEnvelope updated_envelope = *envelope;
      updated_envelope.state = EnvelopeState::RevokePending;
      updated_envelope.reason = updated.reason;
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      updated_envelope.updated_ns = now;
      ++updated_envelope.completions_rejected;
      StagedRecords records;
      stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
      stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeStateChange);
      const Status status = commit_records_locked(records);
      if (!status) {
        emit(events);
        return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
      }
      *attempt = updated;
      *envelope = updated_envelope;
      outcome.accepted = true;
      outcome.reason = updated.reason;
      outcome.resulting_state = updated_envelope.state;
      outcome.explanation = "the backend refused the revoke; the envelope remains revoke-pending";
      push_event_locked(updated.reason, envelope_id, updated_envelope.generation, attempt_id,
                        updated.detail, events);
      emit(events);
      return outcome;
    }
    EnforcementAttempt acknowledged = *attempt;
    acknowledged.acknowledged = true;
    acknowledged.state = AttemptState::Acknowledged;
    acknowledged.reason = ReasonCode::RevokeDispatched;
    acknowledged.completed_ns = now;
    acknowledged.detail = report.detail.empty() ? std::string("revoke acknowledged") : report.detail;
    StagedRecords records;
    stage_attempt(records, acknowledged, JournalRecordType::AttemptAcknowledged);
    const Status status = commit_records_locked(records);
    if (!status) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
    }
    *attempt = acknowledged;
    probe = make_apply_request(*envelope, *attempt);
    probe.rate_ups = 0;
    probe.burst_tokens = 0;
    backend = backend_;
    need_verify = true;
  }
  emit(events);
  events.clear();
  if (!need_verify) {
    return outcome;
  }

  VerificationOutcome verification;
  if (backend == nullptr) {
    verification.status = VerificationStatus::Failed;
    verification.reason = ReasonCode::ApplyVerificationUnknown;
    verification.detail = "no backend session is bound; the effect is UNKNOWN";
  } else {
    verification = backend->verify(probe);
  }
  outcome.verification_performed = true;

  {
    ScopedLock guard(mutex_);
    EnforcementAttempt* attempt = find_attempt_locked(attempt_id);
    RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (attempt == nullptr || envelope == nullptr) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt or envelope");
    }
    if (attempt->state != AttemptState::Acknowledged) {
      EnforcementAttempt updated = *attempt;
      updated.state = AttemptState::LateRejected;
      updated.reason = ReasonCode::AttemptCancelled;
      updated.completed_ns = now;
      updated.detail = "revoke attempt left the in-flight state during readback";
      StagedRecords records;
      stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
      const Status status = commit_records_locked(records);
      if (!status) {
        emit(events);
        return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
      }
      *attempt = updated;
      ++counters_.completions_rejected;
      outcome.rejected_late = true;
      outcome.reason = updated.reason;
      outcome.explanation = updated.detail;
      emit(events);
      return outcome;
    }

    EnforcementAttempt updated = *attempt;
    RateEnvelope updated_envelope = *envelope;
    updated.verified_rate_ups = verification.observed_rate_ups;
    updated.verified_burst_tokens = verification.observed_burst_tokens;
    updated.completed_ns = now;
    updated_envelope.updated_ns = now;
    ++updated_envelope.completions_applied;

    if (verification.status == VerificationStatus::Confirmed && !verification.observed_present) {
      updated.state = AttemptState::Verified;
      updated.verified = true;
      updated.effect_confirmed = true;
      updated.reason = ReasonCode::RevokeVerified;
      updated_envelope.state = EnvelopeState::Revoked;
      updated_envelope.reason = ReasonCode::EnvelopeRevoked;
      updated_envelope.effect_verified = false;
      updated_envelope.requires_revalidation = false;
      updated_envelope.applied_rate_ups = 0;
      updated_envelope.applied_burst_tokens = 0;
      updated_envelope.bucket.tokens = 0;
      updated_envelope.cooldown_until_ns = 0;
      ++counters_.revokes_verified;
      outcome.verified = true;
    } else {
      updated.state = AttemptState::Ambiguous;
      updated.reason = verification.status == VerificationStatus::Failed
                           ? ReasonCode::ApplyVerificationUnknown
                           : ReasonCode::ApplyVerificationMismatch;
      updated_envelope.state = EnvelopeState::RevokePending;
      updated_envelope.reason = updated.reason;
      updated_envelope.requires_revalidation = true;
      updated_envelope.effect_verified = false;
      outcome.disputed = true;
    }
    updated.detail = verification.detail.empty() ? updated.detail : verification.detail;

    StagedRecords records;
    stage_attempt(records, updated, JournalRecordType::AttemptVerified);
    stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeEffectRecorded);
    const Status status = commit_records_locked(records);
    if (!status) {
      emit(events);
      return lifecycle_fail<CompletionOutcome>(status.code(), status.reason(), status.message());
    }
    *attempt = updated;
    *envelope = updated_envelope;
    outcome.accepted = true;
    outcome.reason = updated.reason;
    outcome.resulting_state = updated_envelope.state;
    outcome.explanation = std::string("revoke readback: present=") +
                          (verification.observed_present ? "true" : "false") + " state=" +
                          std::string(to_string_view(updated_envelope.state));
    push_event_locked(updated.reason, envelope_id, updated_envelope.generation, attempt_id,
                      outcome.explanation, events);
  }
  emit(events);
  return outcome;
}

Result<CompletionOutcome> RateGovernor::complete_revoke(EnforcementAttemptId attempt,
                                                        const CompletionReport& report,
                                                        const ActorContext& actor) {
  RevokeRequest request;
  RateEnvelopeId envelope_id;
  {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_fail<CompletionOutcome>(ErrorCode::Fenced, fence,
                                               "actor incarnation is not current");
    }
    EnforcementAttempt* found = find_attempt_locked(attempt);
    if (found == nullptr) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "unknown attempt");
    }
    if (found->kind != AttemptKind::Revoke) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::InvalidArgument,
                                               ReasonCode::MalformedInput,
                                               "attempt is not a revoke attempt");
    }
    envelope_id = found->envelope;
    const RateEnvelope* envelope = find_envelope_locked(envelope_id);
    if (envelope == nullptr) {
      return lifecycle_fail<CompletionOutcome>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                               "attempt references an unknown envelope");
    }
    request = make_revoke_request(*envelope, *found, found->reason);
  }
  return finish_revoke(envelope_id, attempt, request, report);
}

Status RateGovernor::cancel_attempt(EnforcementAttemptId attempt, ReasonCode reason,
                                    const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    EnforcementAttempt* found = find_attempt_locked(attempt);
    if (found == nullptr) {
      return lifecycle_error(ErrorCode::NotFound, ReasonCode::MalformedInput, "unknown attempt");
    }
    if (!attempt_inflight(found->state)) {
      return lifecycle_error(ErrorCode::Conflict, ReasonCode::AttemptCancelled,
                             "attempt is already terminal");
    }
    EnforcementAttempt updated = *found;
    updated.state = AttemptState::Cancelled;
    updated.reason = reason == ReasonCode::None ? ReasonCode::AttemptCancelled : reason;
    updated.completed_ns = clock_->NowNs();
    updated.detail = "cancelled by an operator; it can never publish success";
    StagedRecords records;
    stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
    const Status status = commit_records_locked(records);
    if (!status) {
      return status;
    }
    *found = updated;
    push_event_locked(ReasonCode::AttemptCancelled, updated.envelope,
                      updated.envelope_generation, updated.id, updated.detail, events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Status RateGovernor::abandon_attempt(EnforcementAttemptId attempt, ReasonCode reason,
                                     const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return lifecycle_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    EnforcementAttempt* found = find_attempt_locked(attempt);
    if (found == nullptr) {
      return lifecycle_error(ErrorCode::NotFound, ReasonCode::MalformedInput, "unknown attempt");
    }
    if (!attempt_inflight(found->state)) {
      return lifecycle_error(ErrorCode::Conflict, ReasonCode::AttemptDeadlineExceeded,
                             "attempt is already terminal");
    }
    EnforcementAttempt updated = *found;
    updated.state = AttemptState::Abandoned;
    updated.reason = reason == ReasonCode::None ? ReasonCode::AttemptAbandonedWorkerDeath : reason;
    updated.completed_ns = clock_->NowNs();
    updated.detail = "abandoned; the effect is UNKNOWN until it is revalidated";
    RateEnvelope* envelope = find_envelope_locked(updated.envelope);
    if (envelope == nullptr) {
      return lifecycle_error(ErrorCode::NotFound, ReasonCode::MalformedInput,
                             "attempt references an unknown envelope");
    }
    // An abandoned attempt may or may not have taken effect at the backend, so
    // nothing may be assumed in either direction.
    RateEnvelope updated_envelope = *envelope;
    updated_envelope.state = EnvelopeState::Degraded;
    updated_envelope.reason = updated.reason;
    updated_envelope.requires_revalidation = true;
    updated_envelope.effect_verified = false;
    updated_envelope.updated_ns = updated.completed_ns;
    StagedRecords records;
    stage_attempt(records, updated, JournalRecordType::AttemptTerminated);
    stage_envelope(records, updated_envelope, JournalRecordType::EnvelopeStateChange);
    const Status status = commit_records_locked(records);
    if (!status) {
      return status;
    }
    *found = updated;
    *envelope = updated_envelope;
    push_event_locked(ReasonCode::AttemptAbandonedWorkerDeath, updated.envelope,
                      updated.envelope_generation, updated.id, updated.detail, events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Result<u64> RateGovernor::consume_burst(RateEnvelopeId id, u64 tokens) {
  const TimestampNs now = clock_->NowNs();
  std::vector<AuditEvent> events;
  Result<u64> result = [&]() -> Result<u64> {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return lifecycle_fail<u64>(ErrorCode::ShuttingDown, ReasonCode::WorkRejectedDuringShutdown,
                                 "engine is shutting down");
    }
    RateEnvelope* envelope = find_envelope_locked(id);
    if (envelope == nullptr) {
      return lifecycle_fail<u64>(ErrorCode::NotFound, ReasonCode::MalformedInput,
                                 "unknown envelope");
    }
    if (envelope->state != EnvelopeState::Applied && envelope->state != EnvelopeState::Degraded) {
      return lifecycle_fail<u64>(ErrorCode::Denied, ReasonCode::EnvelopeFailed,
                                 "burst cannot be spent on an envelope without a verified effect");
    }
    if (authority_status_locked(*envelope, now) != ReasonCode::None) {
      return lifecycle_fail<u64>(ErrorCode::StaleAuthority, ReasonCode::EnvelopeStaleNoLiveAuthority,
                                 "the funding authority for this envelope is no longer current");
    }
    RateEnvelope updated = *envelope;
    const RefillOutcome refill = refill_bucket(updated.bucket, now);
    if (refill.clock_regressed) {
      ++counters_.clock_regressions;
    }
    if (tokens > updated.bucket.tokens) {
      ++counters_.token_starvations;
      // The refill that was computed is still persisted: time passed, and that
      // is true regardless of whether the request could be satisfied.
      StagedRecords records;
      stage_envelope(records, updated, JournalRecordType::EnvelopeCounters);
      const Status status = commit_records_locked(records);
      if (!status) {
        return lifecycle_fail<u64>(status.code(), status.reason(), status.message());
      }
      *envelope = updated;
      return lifecycle_fail<u64>(ErrorCode::Denied, ReasonCode::TokenBucketEmpty,
                                 "burst allowance is exhausted");
    }
    (void)try_consume(updated.bucket, tokens);
    updated.updated_ns = now;
    ++counters_.token_consumptions;
    StagedRecords records;
    stage_envelope(records, updated, JournalRecordType::EnvelopeCounters);
    const Status status = commit_records_locked(records);
    if (!status) {
      return lifecycle_fail<u64>(status.code(), status.reason(), status.message());
    }
    *envelope = updated;
    return updated.bucket.tokens;
  }();
  emit(events);
  return result;
}

}  // namespace rate_governor
