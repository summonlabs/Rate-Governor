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

inline constexpr usize kMaxRecordsPerTransaction = 256;

Error make_error_status(ErrorCode code, ReasonCode reason, std::string message) {
  return make_error(code, reason, std::move(message));
}

template <class T>
Result<T> fail(ErrorCode code, ReasonCode reason, std::string message) {
  return Result<T>(make_error_status(code, reason, std::move(message)));
}

bool is_inflight(AttemptState state) {
  return state == AttemptState::Created || state == AttemptState::Dispatched ||
         state == AttemptState::Acknowledged;
}

}  // namespace

IEventSink::~IEventSink() = default;

std::string_view to_string_view(DurabilityMode mode) noexcept {
  switch (mode) {
    case DurabilityMode::None: return "None";
    case DurabilityMode::Strict: return "Strict";
  }
  return "Unknown";
}

RateGovernor::RateGovernor(EngineConfig config, IClock& clock, IEnforcementBackend* backend,
                           Journal* journal, IEventSink* sink)
    : config_(std::move(config)), clock_(&clock), backend_(backend), journal_(journal), sink_(sink) {
  // The configured epoch is the incarnation's starting point. Ignoring it
  // would let a fresh process believe it is epoch 1 and be fenced by its own
  // past, or worse, fence a newer incarnation.
  epoch_ = config_.initial_epoch;
  if (!epoch_.valid()) {
    epoch_ = FabricEpoch(1);
  }
  if (!config_.boot.valid()) {
    config_.boot = WorkerBootId(1);
  }
  if (config_.max_attribute_bytes == 0) {
    config_.max_attribute_bytes = 256;
  }
  if (config_.max_audit_events == 0) {
    config_.max_audit_events = 1;
  }
  if (config_.max_attempts_per_envelope == 0) {
    config_.max_attempts_per_envelope = 1;
  }
  if (backend_ != nullptr) {
    backend_session_epoch_ = backend_->session_epoch();
    backend_bound_ = backend_->session_live() && backend_session_epoch_ == epoch_;
  }
  if (journal_ != nullptr && journal_->open() && config_.durability == DurabilityMode::Strict) {
    write_incarnation_banner_locked();
  }
}

RateGovernor::~RateGovernor() = default;

void RateGovernor::write_incarnation_banner_locked() {
  // A durable incarnation banner makes the fabric epoch recoverable. Without
  // it, an engine that was constructed at epoch 5 and crashed would recover at
  // epoch 1 and could be fenced by its own past.
  ByteWriter writer(32);
  encode_epoch_payload(writer, epoch_, config_.boot, ReasonCode::EngineShutdown);
  const Status status = commit_one_locked(JournalRecordType::EpochAdvance, writer.data());
  if (!status) {
    durability_message_ = status.message();
  }
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------
bool RateGovernor::journaling_locked() const {
  return journal_ != nullptr && journal_->open() && config_.durability == DurabilityMode::Strict &&
         !durability_degraded_;
}

Status RateGovernor::durable_status_locked(JournalStatus status, const std::string& message) {
  durability_degraded_ = true;
  durability_message_ = std::string("journal ") + std::string(to_string_view(status)) + ": " +
                        message;
  ++counters_.journal_failures;
  return Status(make_error_status(ErrorCode::JournalFailure, ReasonCode::JournalAppendFailed,
                                  durability_message_));
}

Status RateGovernor::commit_records_locked(const StagedRecords& records) {
  if (config_.durability != DurabilityMode::Strict) {
    return Status::ok();
  }
  if (durability_degraded_) {
    return Status(make_error_status(ErrorCode::JournalFailure, ReasonCode::JournalAppendFailed,
                                    durability_message_));
  }
  if (journal_ == nullptr || !journal_->open()) {
    return Status(make_error_status(ErrorCode::JournalFailure, ReasonCode::JournalAppendFailed,
                                    "strict durability requires an open journal"));
  }
  if (records.empty()) {
    return Status::ok();
  }
  const u64 txn = next_txn_id_++;
  JournalResult result = journal_->begin(txn);
  if (!result) {
    (void)journal_->rollback();
    return durable_status_locked(result.status, result.message);
  }
  for (const auto& record : records) {
    result = journal_->append(record.first, record.second);
    if (!result) {
      (void)journal_->rollback();
      return durable_status_locked(result.status, result.message);
    }
  }
  result = journal_->commit();
  if (!result) {
    return durable_status_locked(result.status, result.message);
  }
  ++counters_.journal_commits;
  maybe_compact_locked();
  return Status::ok();
}

Status RateGovernor::commit_one_locked(JournalRecordType type, std::vector<std::byte> payload) {
  StagedRecords records;
  records.emplace_back(type, std::move(payload));
  return commit_records_locked(records);
}

Status RateGovernor::commit_many_locked(const StagedRecords& records) {
  for (usize offset = 0; offset < records.size(); offset += kMaxRecordsPerTransaction) {
    const usize end = std::min(records.size(), offset + kMaxRecordsPerTransaction);
    StagedRecords chunk(records.begin() + static_cast<std::ptrdiff_t>(offset),
                        records.begin() + static_cast<std::ptrdiff_t>(end));
    const Status status = commit_records_locked(chunk);
    if (!status) {
      return status;
    }
  }
  return Status::ok();
}

void RateGovernor::maybe_compact_locked() {
  if (journal_ == nullptr || !journal_->open()) {
    return;
  }
  const u64 limit = journal_->config().max_bytes;
  if (limit == 0 || journal_->size_bytes() <= limit) {
    return;
  }
  std::vector<JournalRecord> snapshot;
  build_snapshot_records_locked(snapshot);
  const JournalResult result = journal_->compact_with(std::move(snapshot));
  if (!result) {
    (void)durable_status_locked(result.status, result.message);
    return;
  }
  ++counters_.compactions;
}

void RateGovernor::build_snapshot_records_locked(std::vector<JournalRecord>& out) const {
  out.clear();
  auto push_entity = [&out](JournalRecordType type, const std::vector<std::byte>& payload) {
    JournalRecord record;
    record.type = type;
    record.payload = payload;
    out.push_back(std::move(record));
  };
  for (const auto& entry : flows_) {
    ByteWriter writer(160);
    encode_flow(writer, entry.second);
    push_entity(JournalRecordType::FlowPut, writer.data());
  }
  for (const auto& entry : grants_) {
    ByteWriter writer(192);
    encode_grant(writer, entry.second);
    push_entity(JournalRecordType::GrantPut, writer.data());
  }
  for (const auto& entry : reservations_) {
    ByteWriter writer(192);
    encode_reservation(writer, entry.second);
    push_entity(JournalRecordType::ReservationPut, writer.data());
  }
  for (const auto& entry : policies_) {
    ByteWriter writer(224);
    encode_policy(writer, entry.second);
    push_entity(JournalRecordType::PolicyPut, writer.data());
  }
  for (const auto& entry : resources_) {
    ByteWriter writer(160);
    encode_resource(writer, entry.second);
    push_entity(JournalRecordType::ResourcePut, writer.data());
  }
  for (const auto& entry : backends_) {
    ByteWriter writer(256);
    encode_backend(writer, entry.second);
    push_entity(JournalRecordType::BackendPut, writer.data());
  }
  {
    ByteWriter writer(32);
    encode_epoch_payload(writer, epoch_, config_.boot, ReasonCode::None);
    push_entity(JournalRecordType::EpochAdvance, writer.data());
  }
  for (const auto& entry : envelopes_) {
    ByteWriter writer(1024);
    encode_envelope(writer, entry.second);
    push_entity(JournalRecordType::EnvelopeOpened, writer.data());
  }
  for (const auto& entry : attempts_) {
    ByteWriter writer(768);
    encode_attempt(writer, entry.second);
    push_entity(JournalRecordType::AttemptCreated, writer.data());
  }
}

std::vector<JournalRecord> RateGovernor::snapshot_records() const {
  ScopedLock guard(mutex_);
  std::vector<JournalRecord> records;
  build_snapshot_records_locked(records);
  return records;
}

void RateGovernor::apply_snapshot_records(const std::vector<JournalRecord>& records) {
  ScopedLock guard(mutex_);
  for (const JournalRecord& record : records) {
    ByteReader reader(record.payload);
    switch (record.type) {
      case JournalRecordType::FileHeader:
      case JournalRecordType::TxnBegin:
      case JournalRecordType::TxnCommit:
      case JournalRecordType::CompactionMarker:
        break;
      case JournalRecordType::FlowPut: {
        Flow flow;
        if (decode_flow(reader, flow)) {
          flows_[flow.id.value()] = flow;
        }
        break;
      }
      case JournalRecordType::GrantPut:
      case JournalRecordType::GrantStateChange: {
        Grant grant;
        if (decode_grant(reader, grant)) {
          grants_[grant.id.value()] = grant;
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      case JournalRecordType::ReservationPut:
      case JournalRecordType::ReservationStateChange: {
        Reservation reservation;
        if (decode_reservation(reader, reservation)) {
          reservations_[reservation.id.value()] = reservation;
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      case JournalRecordType::PolicyPut:
      case JournalRecordType::PolicyStateChange: {
        Policy policy;
        if (decode_policy(reader, policy)) {
          policies_[policy.id.value()] = policy;
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      case JournalRecordType::ResourcePut:
      case JournalRecordType::ResourceStateChange: {
        Resource resource;
        if (decode_resource(reader, resource)) {
          resources_[resource.id.value()] = resource;
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      case JournalRecordType::BackendPut: {
        BackendDescriptor backend;
        if (decode_backend(reader, backend)) {
          backends_[backend.id.value()] = backend;
        }
        break;
      }
      case JournalRecordType::EpochAdvance: {
        FabricEpoch epoch;
        WorkerBootId boot;
        ReasonCode reason = ReasonCode::None;
        if (decode_epoch_payload(reader, epoch, boot, reason) && epoch.valid() &&
            epoch > epoch_) {
          epoch_ = epoch;
        }
        break;
      }
      case JournalRecordType::EnvelopeOpened:
      case JournalRecordType::EnvelopeBindingChange:
      case JournalRecordType::EnvelopeAuthorized:
      case JournalRecordType::EnvelopeStateChange:
      case JournalRecordType::EnvelopeEffectRecorded: {
        RateEnvelope envelope;
        if (decode_envelope(reader, envelope)) {
          const u64 id = envelope.id.value();
          envelopes_[id] = std::move(envelope);
          if (id >= next_envelope_id_) {
            next_envelope_id_ = id + 1;
          }
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      case JournalRecordType::EnvelopeCounters: {
        // A partial record: it patches the token accounting of an envelope that
        // must already exist. A patch without its object is reported as an
        // orphan, never guessed at.
        RateEnvelope patch;
        if (!decode_bucket_payload(reader, patch)) {
          ++recovery_orphan_records_;
          break;
        }
        const auto found = envelopes_.find(patch.id.value());
        if (found == envelopes_.end()) {
          ++recovery_orphan_records_;
          break;
        }
        if (found->second.generation != patch.generation) {
          // The patch describes an older envelope generation. Applying it would
          // resurrect superseded accounting, so it is discarded and reported.
          ++recovery_orphan_records_;
          break;
        }
        found->second.bucket = patch.bucket;
        found->second.created_ns = patch.created_ns;
        found->second.updated_ns = patch.updated_ns;
        found->second.attempts_issued = patch.attempts_issued;
        found->second.completions_applied = patch.completions_applied;
        found->second.completions_rejected = patch.completions_rejected;
        break;
      }
      case JournalRecordType::AttemptCreated:
      case JournalRecordType::AttemptDispatched:
      case JournalRecordType::AttemptAcknowledged:
      case JournalRecordType::AttemptTerminated:
      case JournalRecordType::AttemptVerified:
      case JournalRecordType::AttemptCompensating: {
        EnforcementAttempt attempt;
        if (decode_attempt(reader, attempt)) {
          const u64 id = attempt.id.value();
          if (id >= next_attempt_id_) {
            next_attempt_id_ = id + 1;
          }
          (void)retain_attempt_locked(attempt);
        } else {
          ++recovery_orphan_records_;
        }
        break;
      }
      default:
        ++recovery_orphan_records_;
        break;
    }
  }
}

void RateGovernor::stage_envelope(StagedRecords& records, const RateEnvelope& envelope,
                                  JournalRecordType type) const {
  ByteWriter writer(1024);
  encode_envelope(writer, envelope);
  records.emplace_back(type, writer.data());
}

void RateGovernor::stage_attempt(StagedRecords& records, const EnforcementAttempt& attempt,
                                 JournalRecordType type) const {
  ByteWriter writer(768);
  encode_attempt(writer, attempt);
  records.emplace_back(type, writer.data());
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void RateGovernor::push_event_locked(ReasonCode reason, RateEnvelopeId envelope,
                                     Generation generation, EnforcementAttemptId attempt,
                                     std::string detail, std::vector<AuditEvent>& out) {
  AuditEvent event;
  event.sequence = AuditSequence(next_event_sequence_++);
  event.at_ns = clock_->NowNs();
  event.epoch = epoch_;
  event.boot = config_.boot;
  event.reason = reason;
  event.klass = classify(reason);
  event.envelope = envelope;
  event.envelope_generation = generation;
  event.attempt = attempt;
  event.detail = truncate_bounded(std::move(detail), config_.max_attribute_bytes);
  events_.push_back(event);
  while (events_.size() > config_.max_audit_events) {
    events_.pop_front();
    ++events_dropped_;
    ++counters_.events_dropped;
  }
  out.push_back(event);
  ++counters_.events_emitted;
}

void RateGovernor::emit(const std::vector<AuditEvent>& events) {
  // deliver_events defers the emission when this thread still holds the state
  // lock, so a sink can always re-enter the engine for read-only queries.
  deliver_events(sink_, events);
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------
RateEnvelope* RateGovernor::find_envelope_locked(RateEnvelopeId id) {
  const auto found = envelopes_.find(id.value());
  return found == envelopes_.end() ? nullptr : &found->second;
}

const RateEnvelope* RateGovernor::find_envelope_locked(RateEnvelopeId id) const {
  const auto found = envelopes_.find(id.value());
  return found == envelopes_.end() ? nullptr : &found->second;
}

EnforcementAttempt* RateGovernor::find_attempt_locked(EnforcementAttemptId id) {
  const auto found = attempts_.find(id.value());
  return found == attempts_.end() ? nullptr : &found->second;
}

bool RateGovernor::check_actor_locked(const ActorContext& actor, ReasonCode& reason) const {
  if (!actor.has_incarnation()) {
    reason = ReasonCode::UnknownAuthority;
    return false;
  }
  if (actor.epoch != epoch_) {
    reason = actor.epoch < epoch_ ? ReasonCode::AttemptFencedByEpoch : ReasonCode::UnknownAuthority;
    return false;
  }
  if (actor.boot != config_.boot) {
    reason = ReasonCode::AttemptFencedByBoot;
    return false;
  }
  if (!actor.provenance.authoritative()) {
    reason = ReasonCode::UnknownAuthority;
    return false;
  }
  return true;
}

bool RateGovernor::retain_attempt_locked(const EnforcementAttempt& attempt) {
  attempts_[attempt.id.value()] = attempt;
  std::vector<u64>& list = attempts_by_envelope_[attempt.envelope.value()];
  if (std::find(list.begin(), list.end(), attempt.id.value()) == list.end()) {
    list.push_back(attempt.id.value());
    attempt_order_.push_back(attempt.id.value());
  }
  while (list.size() > config_.max_attempts_per_envelope) {
    bool evicted = false;
    for (usize index = 0; index < list.size(); ++index) {
      const auto found = attempts_.find(list[index]);
      if (found != attempts_.end() && attempt_state_is_terminal(found->second.state)) {
        const u64 victim = list[index];
        attempts_.erase(found);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
        attempt_order_.erase(std::remove(attempt_order_.begin(), attempt_order_.end(), victim),
                             attempt_order_.end());
        evicted = true;
        break;
      }
    }
    if (!evicted) {
      break;
    }
  }
  while (attempt_order_.size() > config_.max_envelopes * config_.max_attempts_per_envelope) {
    const u64 victim = attempt_order_.front();
    attempt_order_.erase(attempt_order_.begin());
    attempts_.erase(victim);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Authority ingest
// ---------------------------------------------------------------------------
namespace {

Status ingest_precondition(ReasonCode structure, const char* what) {
  if (structure == ReasonCode::None) {
    return Status::ok();
  }
  ErrorCode code = (structure == ReasonCode::UnknownAuthority) ? ErrorCode::Denied
                                                               : ErrorCode::InvalidArgument;
  return Status(make_error_status(code, structure, std::string("malformed ") + what));
}

}  // namespace

Status RateGovernor::put_flow(const Flow& flow) {
  const ReasonCode structure = validate_structure(flow);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "flow");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = flows_.find(flow.id.value());
    if (existing != flows_.end() && flow.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::EnvelopeStaleFlowGenerationChanged,
                                      "flow generation is not newer than the stored one"));
    }
    if (existing == flows_.end() && flows_.size() >= config_.max_flows) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "flow table is full"));
    }
    ByteWriter writer(160);
    encode_flow(writer, flow);
    const Status status = commit_one_locked(JournalRecordType::FlowPut, writer.data());
    if (!status) {
      return status;
    }
    flows_[flow.id.value()] = flow;
    push_event_locked(ReasonCode::EnvelopeOpened, RateEnvelopeId{}, Generation{}, 
                      EnforcementAttemptId{}, describe(flow), events);
  }
  emit(events);
  return Status::ok();
}

Status RateGovernor::put_grant(const Grant& grant) {
  const ReasonCode structure = validate_structure(grant);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "grant");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = grants_.find(grant.id.value());
    if (existing != grants_.end() && grant.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::PlanRejectedGrantGenerationMismatch,
                                      "grant generation is not newer than the stored one"));
    }
    if (existing == grants_.end() && grants_.size() >= config_.max_grants) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "grant table is full"));
    }
    ByteWriter writer(192);
    encode_grant(writer, grant);
    const Status status = commit_one_locked(JournalRecordType::GrantPut, writer.data());
    if (!status) {
      return status;
    }
    grants_[grant.id.value()] = grant;
    push_event_locked(ReasonCode::PlanAuthorized, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{}, describe(grant), events);
  }
  emit(events);
  return Status::ok();
}

Status RateGovernor::put_reservation(const Reservation& reservation) {
  const ReasonCode structure = validate_structure(reservation);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "reservation");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = reservations_.find(reservation.id.value());
    if (existing != reservations_.end() &&
        reservation.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::PlanRejectedReservationGenerationMismatch,
                                      "reservation generation is not newer than the stored one"));
    }
    if (existing == reservations_.end() && reservations_.size() >= config_.max_reservations) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "reservation table is full"));
    }
    ByteWriter writer(192);
    encode_reservation(writer, reservation);
    const Status status = commit_one_locked(JournalRecordType::ReservationPut, writer.data());
    if (!status) {
      return status;
    }
    reservations_[reservation.id.value()] = reservation;
    push_event_locked(ReasonCode::PlanAuthorized, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{}, describe(reservation), events);
  }
  emit(events);
  return Status::ok();
}

Status RateGovernor::put_policy(const Policy& policy) {
  const ReasonCode structure = validate_structure(policy);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "policy");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = policies_.find(policy.id.value());
    if (existing != policies_.end() && policy.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::PlanRejectedPolicyGenerationMismatch,
                                      "policy generation is not newer than the stored one"));
    }
    if (existing == policies_.end() && policies_.size() >= config_.max_policies) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "policy table is full"));
    }
    ByteWriter writer(224);
    encode_policy(writer, policy);
    const Status status = commit_one_locked(JournalRecordType::PolicyPut, writer.data());
    if (!status) {
      return status;
    }
    policies_[policy.id.value()] = policy;
    push_event_locked(ReasonCode::EnvelopeReducedByPolicy, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{}, describe(policy), events);
  }
  emit(events);
  return Status::ok();
}

Status RateGovernor::put_resource(const Resource& resource) {
  const ReasonCode structure = validate_structure(resource);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "resource");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = resources_.find(resource.id.value());
    if (existing != resources_.end() && resource.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::PlanRejectedResourceGenerationMismatch,
                                      "resource generation is not newer than the stored one"));
    }
    if (existing == resources_.end() && resources_.size() >= config_.max_resources) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "resource table is full"));
    }
    ByteWriter writer(160);
    encode_resource(writer, resource);
    const Status status = commit_one_locked(JournalRecordType::ResourcePut, writer.data());
    if (!status) {
      return status;
    }
    resources_[resource.id.value()] = resource;
    push_event_locked(ReasonCode::EnvelopeReducedByPolicy, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{}, describe(resource), events);
  }
  emit(events);
  return Status::ok();
}

Status RateGovernor::put_backend(const BackendDescriptor& backend) {
  const ReasonCode structure = validate_structure(backend);
  if (structure != ReasonCode::None) {
    return ingest_precondition(structure, "backend descriptor");
  }
  std::vector<AuditEvent> events;
  {
    ScopedLock guard(mutex_);
    if (shutting_down_.load(std::memory_order_acquire)) {
      ++counters_.work_rejected_shutdown;
      return Status(make_error_status(ErrorCode::ShuttingDown,
                                      ReasonCode::WorkRejectedDuringShutdown,
                                      "engine is shutting down"));
    }
    const auto existing = backends_.find(backend.id.value());
    if (existing != backends_.end() && backend.generation <= existing->second.generation) {
      return Status(make_error_status(ErrorCode::StaleAuthority,
                                      ReasonCode::PlanRejectedBackendGenerationMismatch,
                                      "backend generation is not newer than the stored one"));
    }
    if (existing == backends_.end() && backends_.size() >= config_.max_backends) {
      return Status(make_error_status(ErrorCode::LimitExceeded, ReasonCode::CapacityLimitExceeded,
                                      "backend table is full"));
    }
    ByteWriter writer(256);
    encode_backend(writer, backend);
    const Status status = commit_one_locked(JournalRecordType::BackendPut, writer.data());
    if (!status) {
      return status;
    }
    backends_[backend.id.value()] = backend;
    push_event_locked(ReasonCode::BackendSessionEstablished, RateEnvelopeId{}, Generation{},
                      EnforcementAttemptId{}, describe(backend), events);
  }
  emit(events);
  return Status::ok();
}

}  // namespace rate_governor
