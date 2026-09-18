#ifndef RATE_GOVERNOR_JOURNAL_HPP
#define RATE_GOVERNOR_JOURNAL_HPP

// Crash-safe, versioned, integrity-checked durable record log.
//
// Protocol per mutation:
//   validate -> bind authority -> plan -> reserve -> journal/temp state ->
//   perform work -> verify -> commit -> retire/cleanup
//
// A mutation is durable only when its transaction commit record and the
// payload records it covers are both on stable storage, which is why commit()
// flushes and (by default) calls the platform durability barrier before it
// reports success. A transaction that never reaches its commit record is
// discarded on recovery and reported as unfinished rather than replayed.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rate_governor/checked_math.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

enum class JournalRecordType : std::uint16_t {
  Unknown = 0,
  // Format banner: version, engine identity, build description.
  FileHeader = 1,
  // Authority inputs
  FlowPut = 9,
  GrantPut = 10,
  GrantStateChange = 11,
  ReservationPut = 12,
  ReservationStateChange = 13,
  PolicyPut = 14,
  PolicyStateChange = 15,
  ResourcePut = 16,
  ResourceStateChange = 17,
  BackendPut = 18,
  EpochAdvance = 20,
  // Envelope lifecycle
  EnvelopeOpened = 30,
  EnvelopeBindingChange = 31,
  EnvelopeAuthorized = 32,
  EnvelopeStateChange = 33,
  EnvelopeEffectRecorded = 34,
  EnvelopeCounters = 35,
  // Attempts
  AttemptCreated = 40,
  AttemptDispatched = 41,
  AttemptAcknowledged = 42,
  AttemptTerminated = 43,
  AttemptVerified = 44,
  AttemptCompensating = 45,
  // Transaction control
  TxnBegin = 60,
  TxnCommit = 61,
  // Compaction marker: everything before this record is superseded by the
  // snapshot records that follow it inside the same transaction.
  CompactionMarker = 62,
};

[[nodiscard]] std::string_view to_string_view(JournalRecordType type) noexcept;

struct JournalRecord {
  JournalRecordType type{JournalRecordType::Unknown};
  u64 sequence{0};
  u64 txn_id{0};
  std::vector<std::byte> payload;
};

enum class JournalStatus : std::uint8_t {
  Ok = 0,
  NotOpen,
  IoError,
  // Framing is intact but the record body failed its integrity check, or the
  // framing itself is impossible (bad magic, impossible length).
  Corrupt,
  // The file ends in the middle of a record. Expected after a crash; the tail
  // is discarded rather than repaired.
  Truncated,
  VersionUnsupported,
  BoundsExceeded,
  NoTransaction,
  TransactionOpen,
  PayloadTooLarge,
};

[[nodiscard]] std::string_view to_string_view(JournalStatus status) noexcept;

struct JournalConfig {
  std::string path;
  // Compaction is triggered by the engine when the log exceeds this size.
  u64 max_bytes{64ULL * 1024ULL * 1024ULL};
  // Per-record payload bound; oversized payloads are refused, never truncated.
  u64 max_payload_bytes{1ULL * 1024ULL * 1024ULL};
  bool fsync_on_commit{true};
};

struct JournalResult {
  bool ok{false};
  JournalStatus status{JournalStatus::Ok};
  u64 sequence{0};
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

// Append-only writer. Not thread safe by design: the engine serialises all
// durable mutation under one lock and orders the journal identically.
class Journal {
 public:
  Journal() = default;
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  [[nodiscard]] static std::unique_ptr<Journal> open(const JournalConfig& config, bool create_new,
                                                     JournalResult& result);

  [[nodiscard]] JournalResult begin(u64 txn_id);
  [[nodiscard]] JournalResult append(JournalRecordType type, std::span<const std::byte> payload);
  [[nodiscard]] JournalResult commit();
  [[nodiscard]] JournalResult rollback();

  [[nodiscard]] bool flush();
  [[nodiscard]] u64 size_bytes() const noexcept { return size_bytes_; }
  [[nodiscard]] u64 record_count() const noexcept { return record_count_; }
  [[nodiscard]] u64 last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] u64 next_sequence() const noexcept { return last_sequence_ + 1; }
  [[nodiscard]] const std::string& path() const noexcept { return config_.path; }
  [[nodiscard]] bool open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] const JournalConfig& config() const noexcept { return config_; }
  [[nodiscard]] bool transaction_open() const noexcept { return txn_open_; }

  void close() noexcept;

  // Replaces the log with a compacted snapshot: writes the supplied committed
  // records to a temporary file, flushes it to stable storage and atomically
  // renames it over the live log. Never leaves a partially written log.
  [[nodiscard]] JournalResult compact_with(std::vector<JournalRecord> snapshot_records);

 private:
  [[nodiscard]] JournalResult write_record(JournalRecordType type, u64 txn_id,
                                           std::span<const std::byte> payload);
  [[nodiscard]] bool write_all(const void* data, usize size);

  JournalConfig config_{};
  std::FILE* file_{nullptr};
  u64 size_bytes_{0};
  u64 last_sequence_{0};
  u64 record_count_{0};
  u64 txn_id_{0};
  bool txn_open_{false};
  u64 txn_record_count_{0};
  u64 txn_bytes_{0};
};

struct JournalScanResult {
  bool ok{false};
  JournalStatus status{JournalStatus::Ok};
  std::vector<JournalRecord> committed;
  u64 committed_transactions{0};
  u64 discarded_uncommitted_records{0};
  u64 discarded_truncated_bytes{0};
  u64 last_sequence{0};
  bool truncated_tail{false};
  bool has_file_header{false};
  std::string message;
};

// Reads a journal from disk and returns only records covered by a commit
// record. A torn tail is reported and discarded; corrupt bodies are reported
// and stop the scan at the last intact transaction.
[[nodiscard]] JournalScanResult scan_journal_file(const std::string& path);

// Serialises records into the on-disk framing (used by tests and by the
// compaction path). Exposed so the framing itself can be validated directly.
[[nodiscard]] std::vector<std::byte> encode_journal_record(const JournalRecord& record,
                                                           u64 payload_bound);
[[nodiscard]] bool decode_journal_record(std::span<const std::byte> bytes, usize offset,
                                         JournalRecord& out, usize& consumed,
                                         JournalStatus& status, u64 payload_bound) noexcept;

inline constexpr u32 kJournalRecordMagic = 0x314A4752U;  // "RGJ1"
inline constexpr usize kJournalRecordHeaderSize = 36;
// Refuse to scan an implausibly large journal instead of allocating for it.
inline constexpr u64 kJournalMaxScanBytes = 512ULL * 1024ULL * 1024ULL;

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_JOURNAL_HPP
