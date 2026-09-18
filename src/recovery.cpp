#include "rate_governor/recovery.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "engine_lock.hpp"
#include "rate_governor/codec.hpp"
#include "rate_governor/plan.hpp"
#include "record_codec.hpp"

namespace rate_governor {
namespace {

// The highest fabric epoch any committed record mentions. A recovery that
// guessed lower would let a past incarnation fence its own successor.
FabricEpoch highest_recorded_epoch(const std::vector<JournalRecord>& records) {
  FabricEpoch highest{};
  for (const JournalRecord& record : records) {
    ByteReader reader(record.payload);
    if (record.type == JournalRecordType::EpochAdvance) {
      FabricEpoch epoch;
      WorkerBootId boot;
      ReasonCode reason = ReasonCode::None;
      if (decode_epoch_payload(reader, epoch, boot, reason) && epoch > highest) {
        highest = epoch;
      }
      continue;
    }
    if (record.type == JournalRecordType::EnvelopeOpened ||
        record.type == JournalRecordType::EnvelopeAuthorized ||
        record.type == JournalRecordType::EnvelopeStateChange ||
        record.type == JournalRecordType::EnvelopeBindingChange ||
        record.type == JournalRecordType::EnvelopeEffectRecorded) {
      RateEnvelope envelope;
      if (decode_envelope(reader, envelope)) {
        if (envelope.applied_epoch > highest) {
          highest = envelope.applied_epoch;
        }
        if (envelope.has_plan && envelope.plan.fingerprint.epoch > highest) {
          highest = envelope.plan.fingerprint.epoch;
        }
      }
      continue;
    }
    if (record.type == JournalRecordType::AttemptCreated ||
        record.type == JournalRecordType::AttemptDispatched ||
        record.type == JournalRecordType::AttemptAcknowledged ||
        record.type == JournalRecordType::AttemptTerminated ||
        record.type == JournalRecordType::AttemptVerified) {
      EnforcementAttempt attempt;
      if (decode_attempt(reader, attempt) && attempt.epoch > highest) {
        highest = attempt.epoch;
      }
      continue;
    }
  }
  return highest;
}

// The previous *coordinator* incarnation. Only incarnation banner records
// carry it: an attempt's boot id names the enforcement process, which is a
// different identity and must not be mistaken for a coordinator incarnation.
WorkerBootId highest_recorded_boot(const std::vector<JournalRecord>& records) {
  WorkerBootId highest{};
  for (const JournalRecord& record : records) {
    if (record.type != JournalRecordType::EpochAdvance) {
      continue;
    }
    ByteReader reader(record.payload);
    FabricEpoch epoch;
    WorkerBootId boot;
    ReasonCode reason = ReasonCode::None;
    if (decode_epoch_payload(reader, epoch, boot, reason) && boot > highest) {
      highest = boot;
    }
  }
  return highest;
}

}  // namespace

Result<RecoveredEngine> recover_engine(const std::string& journal_path, EngineConfig config,
                                       IClock& clock, IEventSink* sink, bool truncate_journal_tail) {
  RecoveredEngine recovered;

  JournalScanResult scan = scan_journal_file(journal_path);
  if (!scan.ok) {
    return Result<RecoveredEngine>(make_error(ErrorCode::JournalFailure,
                                              scan.status == JournalStatus::VersionUnsupported
                                                  ? ReasonCode::JournalVersionUnsupported
                                                  : ReasonCode::JournalCorruptRecord,
                                              scan.message));
  }

  RecoveryReport& report = recovered.report;
  report.journal_present = true;
  report.journal_ok = true;
  report.status = scan.status;
  report.committed_records = static_cast<u64>(scan.committed.size());
  report.committed_transactions = scan.committed_transactions;
  report.discarded_uncommitted_records = scan.discarded_uncommitted_records;
  report.discarded_truncated_bytes = scan.discarded_truncated_bytes;
  report.truncated_tail = scan.truncated_tail;
  report.liveness_restored = false;
  report.backend_bound = false;

  const FabricEpoch recorded_epoch = highest_recorded_epoch(scan.committed);
  report.previous_boot = highest_recorded_boot(scan.committed);

  EngineConfig effective = config;
  effective.initial_epoch = recorded_epoch.valid() ? recorded_epoch : config.initial_epoch;
  report.recovered_epoch = effective.initial_epoch;
  report.new_boot = effective.boot;

  JournalConfig journal_config = effective.journal;
  if (journal_config.path.empty()) {
    journal_config.path = journal_path;
  }
  JournalResult open_result;
  std::unique_ptr<Journal> journal = Journal::open(journal_config, false, open_result);
  if (!journal) {
    return Result<RecoveredEngine>(
        make_error(ErrorCode::JournalFailure, ReasonCode::JournalAppendFailed,
                   std::string("cannot reopen the journal for append: ") + open_result.message));
  }
  effective.durability = DurabilityMode::Strict;

  recovered.journal = std::move(journal);
  recovered.engine = std::make_unique<RateGovernor>(effective, clock, nullptr,
                                                    recovered.journal.get(), sink);

  recovered.engine->apply_snapshot_records(scan.committed);

  const ActorContext actor = operator_context(recovered.engine->epoch(), effective.boot,
                                              "recovery of a previous incarnation");
  u64 attempts_marked = 0;
  u64 envelopes_demoted = 0;
  u64 effects_demoted = 0;
  const Status demoted = recovered.engine->demote_for_recovery(actor, attempts_marked,
                                                               envelopes_demoted, effects_demoted);
  if (!demoted) {
    return Result<RecoveredEngine>(demoted.error());
  }
  report.attempts_marked_ambiguous = attempts_marked;
  report.envelopes_requiring_revalidation = envelopes_demoted;
  report.effect_claims_demoted = effects_demoted;

  if (effective.advance_epoch_on_recovery) {
    const FabricEpoch next(recovered.engine->epoch().value() + 1);
    const Status advanced = recovered.engine->advance_epoch(next, actor);
    if (!advanced) {
      return Result<RecoveredEngine>(advanced.error());
    }
  }
  report.new_epoch = recovered.engine->epoch();

  const EngineStorageStats stats = recovered.engine->storage_stats();
  report.grants = stats.grants;
  report.reservations = stats.reservations;
  report.policies = stats.policies;
  report.resources = stats.resources;
  report.backends = stats.backends;
  report.envelopes = stats.envelopes;
  report.attempts = stats.attempts;
  report.envelopes_revoke_pending = 0;
  {
    const std::vector<EnvelopeView> views =
        recovered.engine->list_envelopes(static_cast<usize>(stats.envelopes));
    for (const EnvelopeView& view : views) {
      if (view.state == EnvelopeState::RevokePending) {
        ++report.envelopes_revoke_pending;
      }
    }
  }

  if (truncate_journal_tail && scan.truncated_tail) {
    const std::vector<JournalRecord> snapshot = recovered.engine->snapshot_records();
    Journal* raw = recovered.journal.get();
    const JournalResult compacted = raw->compact_with(snapshot);
    AuditEvent note;
    note.reason = ReasonCode::JournalCompacted;
    note.detail = compacted ? "truncated journal tail was dropped by compaction"
                            : std::string("tail truncation failed: ") + compacted.message;
    report.notes.push_back(std::move(note));
  }

  report.explanation =
      "durable configuration, committed authoritative state and audit history were restored; "
      "unfinished attempts are AMBIGUOUS, every effect claim requires revalidation, and no "
      "process liveness, session, lease or publisher authority was restored";
  return Result<RecoveredEngine>(std::move(recovered));
}

}  // namespace rate_governor
