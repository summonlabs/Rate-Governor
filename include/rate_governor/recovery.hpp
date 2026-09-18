#ifndef RATE_GOVERNOR_RECOVERY_HPP
#define RATE_GOVERNOR_RECOVERY_HPP

// Recovery: what durable state may and may not restore.
//
// Durable configuration and history are restored. Committed authoritative
// state is restored. Unfinished attempts become AMBIGUOUS and can never publish
// success. Every effect claim is demoted to "requires revalidation", because a
// journal entry proves that a decision was taken, never that a backend still
// holds the effect. Process liveness, sessions, leases and publisher authority
// are never restored from disk.

#include <memory>
#include <string>
#include <vector>

#include "rate_governor/engine.hpp"

namespace rate_governor {

struct RecoveryReport {
  bool journal_present{false};
  bool journal_ok{false};
  JournalStatus status{JournalStatus::Ok};
  u64 committed_records{0};
  u64 committed_transactions{0};
  u64 discarded_uncommitted_records{0};
  u64 discarded_truncated_bytes{0};
  bool truncated_tail{false};

  u64 grants{0};
  u64 reservations{0};
  u64 policies{0};
  u64 resources{0};
  u64 backends{0};
  u64 envelopes{0};
  u64 attempts{0};

  u64 attempts_marked_ambiguous{0};
  u64 envelopes_requiring_revalidation{0};
  u64 effect_claims_demoted{0};
  u64 envelopes_revoke_pending{0};

  FabricEpoch recovered_epoch{};
  FabricEpoch new_epoch{};
  WorkerBootId previous_boot{};
  WorkerBootId new_boot{};
  // Always false. Present so the claim is explicit rather than implied.
  bool liveness_restored{false};
  bool backend_bound{false};
  std::string explanation;
  std::vector<AuditEvent> notes;
};

struct RecoveredEngine {
  // Declaration order matters: the engine holds a raw pointer to the journal
  // and is destroyed first.
  std::unique_ptr<Journal> journal;
  std::unique_ptr<RateGovernor> engine;
  RecoveryReport report;
};

// Loads a journal and constructs an engine whose authoritative state is exactly
// what the committed records describe, with every live claim demoted.
[[nodiscard]] Result<RecoveredEngine> recover_engine(const std::string& journal_path,
                                                     EngineConfig config, IClock& clock,
                                                     IEventSink* sink,
                                                     bool truncate_journal_tail = false);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_RECOVERY_HPP
