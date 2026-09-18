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

Status status_error(ErrorCode code, ReasonCode reason, std::string message) {
  return Status(make_error(code, reason, std::move(message)));
}

template <class T>
Result<T> fail_result(ErrorCode code, ReasonCode reason, std::string message) {
  return Result<T>(status_error(code, reason, std::move(message)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Authority state changes. A state change always advances the generation, so
// every envelope bound to the previous generation is detectably stale.
// ---------------------------------------------------------------------------
Status RateGovernor::change_grant_state(GrantId id, Generation generation, GrantState state,
                                        ReasonCode reason, const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return status_error(ErrorCode::ShuttingDown, ReasonCode::WorkRejectedDuringShutdown,
                                 "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return status_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    const auto found = grants_.find(id.value());
    if (found == grants_.end()) {
      return status_error(ErrorCode::NotFound, ReasonCode::PlanRejectedMissingGrant,
                                 "unknown grant");
    }
    if (found->second.generation != generation) {
      return status_error(ErrorCode::StaleAuthority,
                                 ReasonCode::PlanRejectedGrantGenerationMismatch,
                                 "grant generation does not match the current generation");
    }
    Grant updated = found->second;
    updated.generation = Generation(generation.value() + 1);
    updated.state = state;
    updated.provenance = actor.provenance;
    ByteWriter writer(192);
    encode_grant(writer, updated);
    const Status status = commit_one_locked(JournalRecordType::GrantStateChange, writer.data());
    if (!status) {
      return status;
    }
    grants_[id.value()] = updated;
    push_event_locked(reason, RateEnvelopeId{}, Generation{}, EnforcementAttemptId{},
                      describe(updated), events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Status RateGovernor::change_reservation_state(ReservationId id, Generation generation,
                                              ReservationState state, ReasonCode reason,
                                              const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return status_error(ErrorCode::ShuttingDown, ReasonCode::WorkRejectedDuringShutdown,
                                 "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return status_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    const auto found = reservations_.find(id.value());
    if (found == reservations_.end()) {
      return status_error(ErrorCode::NotFound, ReasonCode::PlanRejectedMissingReservation,
                                 "unknown reservation");
    }
    if (found->second.generation != generation) {
      return status_error(ErrorCode::StaleAuthority,
                                 ReasonCode::PlanRejectedReservationGenerationMismatch,
                                 "reservation generation does not match the current generation");
    }
    Reservation updated = found->second;
    updated.generation = Generation(generation.value() + 1);
    updated.state = state;
    updated.provenance = actor.provenance;
    ByteWriter writer(192);
    encode_reservation(writer, updated);
    const Status status =
        commit_one_locked(JournalRecordType::ReservationStateChange, writer.data());
    if (!status) {
      return status;
    }
    reservations_[id.value()] = updated;
    push_event_locked(reason, RateEnvelopeId{}, Generation{}, EnforcementAttemptId{},
                      describe(updated), events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Status RateGovernor::change_policy_state(PolicyId id, Generation generation, PolicyState state,
                                         ReasonCode reason, const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return status_error(ErrorCode::ShuttingDown, ReasonCode::WorkRejectedDuringShutdown,
                                 "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return status_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    const auto found = policies_.find(id.value());
    if (found == policies_.end()) {
      return status_error(ErrorCode::NotFound, ReasonCode::PlanRejectedMissingPolicy,
                          "unknown policy");
    }
    if (found->second.generation != generation) {
      return status_error(ErrorCode::StaleAuthority,
                                 ReasonCode::PlanRejectedPolicyGenerationMismatch,
                                 "policy generation does not match the current generation");
    }
    Policy updated = found->second;
    updated.generation = Generation(generation.value() + 1);
    updated.state = state;
    updated.provenance = actor.provenance;
    ByteWriter writer(224);
    encode_policy(writer, updated);
    const Status status = commit_one_locked(JournalRecordType::PolicyStateChange, writer.data());
    if (!status) {
      return status;
    }
    policies_[id.value()] = updated;
    push_event_locked(reason, RateEnvelopeId{}, Generation{}, EnforcementAttemptId{},
                      describe(updated), events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Status RateGovernor::change_resource_state(ResourceId id, Generation generation,
                                           ResourceState state, ReasonCode reason,
                                           const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return status_error(ErrorCode::ShuttingDown, ReasonCode::WorkRejectedDuringShutdown,
                                 "engine is shutting down");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return status_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    const auto found = resources_.find(id.value());
    if (found == resources_.end()) {
      return status_error(ErrorCode::NotFound, ReasonCode::PlanRejectedUnknownResource,
                                 "unknown resource");
    }
    if (found->second.generation != generation) {
      return status_error(ErrorCode::StaleAuthority,
                                 ReasonCode::PlanRejectedResourceGenerationMismatch,
                                 "resource generation does not match the current generation");
    }
    Resource updated = found->second;
    updated.generation = Generation(generation.value() + 1);
    updated.state = state;
    updated.provenance = actor.provenance;
    ByteWriter writer(160);
    encode_resource(writer, updated);
    const Status status = commit_one_locked(JournalRecordType::ResourceStateChange, writer.data());
    if (!status) {
      return status;
    }
    resources_[id.value()] = updated;
    push_event_locked(reason, RateEnvelopeId{}, Generation{}, EnforcementAttemptId{},
                      describe(updated), events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

Status RateGovernor::advance_epoch(FabricEpoch new_epoch, const ActorContext& actor) {
  std::vector<AuditEvent> events;
  Status result = [&]() -> Status {
    ScopedLock guard(mutex_);
    if (!new_epoch.valid() || new_epoch <= epoch_) {
      return status_error(ErrorCode::InvalidArgument, ReasonCode::MalformedInput,
                                 "a new fabric epoch must be strictly greater than the current one");
    }
    ReasonCode fence = ReasonCode::None;
    if (!check_actor_locked(actor, fence)) {
      ++counters_.fencing_rejections;
      return status_error(ErrorCode::Fenced, fence, "actor incarnation is not current");
    }
    StagedRecords records;
    ByteWriter writer(32);
    encode_epoch_payload(writer, new_epoch, config_.boot, ReasonCode::EnvelopeStaleFabricEpochAdvanced);
    records.emplace_back(JournalRecordType::EpochAdvance, writer.data());

    std::vector<RateEnvelope> updated_envelopes;
    for (auto& entry : envelopes_) {
      RateEnvelope& envelope = entry.second;
      const bool holds_or_claims =
          envelope.effect_verified || envelope.applied_rate_ups != 0 ||
          envelope.state == EnvelopeState::Applied || envelope.state == EnvelopeState::Dispatching;
      if (!holds_or_claims && !envelope.requires_revalidation) {
        continue;
      }
      RateEnvelope updated = envelope;
      updated.requires_revalidation = true;
      updated.effect_verified = false;
      if (updated.state == EnvelopeState::Applied || updated.state == EnvelopeState::Dispatching) {
        updated.state = EnvelopeState::Degraded;
        updated.reason = ReasonCode::EnvelopeStaleFabricEpochAdvanced;
      }
      updated.updated_ns = clock_->NowNs();
      stage_envelope(records, updated, JournalRecordType::EnvelopeStateChange);
      updated_envelopes.push_back(updated);
    }

    const Status status = commit_records_locked(records);
    if (!status) {
      return status;
    }
    epoch_ = new_epoch;
    backend_bound_ = false;
    backend_session_epoch_ = FabricEpoch{};
    for (const RateEnvelope& updated : updated_envelopes) {
      envelopes_[updated.id.value()] = updated;
      ++counters_.stale_transitions;
      push_event_locked(ReasonCode::EnvelopeStaleFabricEpochAdvanced, updated.id, updated.generation,
                        EnforcementAttemptId{}, "fabric epoch advanced; effect requires revalidation",
                        events);
    }
    push_event_locked(ReasonCode::EnvelopeStaleFabricEpochAdvanced, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{},
                      "fabric epoch is now " + new_epoch.to_string() +
                          "; the backend session must be re-admitted",
                      events);
    return Status::ok();
  }();
  emit(events);
  return result;
}

void RateGovernor::bind_backend(IEnforcementBackend* backend, FabricEpoch session_epoch) {
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    backend_ = backend;
    backend_session_epoch_ = session_epoch;
    // Fail closed: a backend that cannot state the incarnation it is serving
    // cannot be fenced, so it is never admitted. Both the caller's claim and
    // the backend's own report must agree with the current fabric epoch.
    const FabricEpoch reported = backend == nullptr ? FabricEpoch{} : backend->session_epoch();
    const bool epoch_agrees = reported.valid() && reported == epoch_ && session_epoch == epoch_;
    backend_bound_ =
        backend != nullptr && backend->session_live() && epoch_agrees;
    std::string detail = "backend bound: ";
    detail.append(backend == nullptr ? std::string("none") : backend->session_description());
    if (!backend_bound_) {
      detail.append(reported.valid()
                        ? " (session epoch does not match the current fabric epoch; not admitted)"
                        : " (backend reports no session epoch; it cannot be fenced and is not "
                          "admitted)");
      ++counters_.fencing_rejections;
    }
    push_event_locked(backend_bound_ ? ReasonCode::BackendSessionEstablished
                                     : ReasonCode::BackendFencedStaleEpoch,
                      RateEnvelopeId{}, Generation{}, EnforcementAttemptId{}, std::move(detail),
                      events);
  }
  emit(events);
}

}  // namespace rate_governor
