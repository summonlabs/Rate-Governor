#include "rate_governor/journal.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rate_governor/codec.hpp"
#include "rate_governor/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rate_governor {
namespace {

// Journal record framing (little endian):
//   [ 0.. 3] magic            u32  kJournalRecordMagic
//   [ 4.. 5] format version   u16  kJournalFormatVersion
//   [ 6.. 7] record type      u16
//   [ 8..11] flags            u32  reserved, must be zero
//   [12..19] sequence         u64  engine-wide monotonic record sequence
//   [20..27] transaction id   u64  0 means "self-committing"
//   [28..31] payload length   u32  bounded by the configured payload bound
//   [32..35] payload crc      u32  CRC-32C over bytes [0..27] followed by the payload
//   [36..  ] payload
inline constexpr usize kHeaderWithoutCrc = 32;
// Layout revision of the record body encoding, independent of the framing
// version. Bumped when a payload encoding changes incompatibly.
inline constexpr u16 kJournalLayoutRevision = 1;

u64 file_size_of(std::FILE* file) {
#if defined(_WIN32)
  const __int64 position = _ftelli64(file);
  return position < 0 ? 0ULL : static_cast<u64>(position);
#else
  const off_t position = ftello(file);
  return position < 0 ? 0ULL : static_cast<u64>(position);
#endif
}

u64 measure_file_size(std::FILE* file) {
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return 0;
  }
  return file_size_of(file);
}

bool durable_barrier(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return false;
  }
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

std::vector<std::byte> commit_payload(u64 txn_id, u64 record_count, u64 byte_count) {
  ByteWriter writer(24);
  writer.u64(txn_id);
  writer.u64(record_count);
  writer.u64(byte_count);
  return writer.data();
}

}  // namespace

std::string_view to_string_view(JournalRecordType type) noexcept {
  switch (type) {
    case JournalRecordType::Unknown: return "Unknown";
    case JournalRecordType::FileHeader: return "FileHeader";
    case JournalRecordType::FlowPut: return "FlowPut";
    case JournalRecordType::GrantPut: return "GrantPut";
    case JournalRecordType::GrantStateChange: return "GrantStateChange";
    case JournalRecordType::ReservationPut: return "ReservationPut";
    case JournalRecordType::ReservationStateChange: return "ReservationStateChange";
    case JournalRecordType::PolicyPut: return "PolicyPut";
    case JournalRecordType::PolicyStateChange: return "PolicyStateChange";
    case JournalRecordType::ResourcePut: return "ResourcePut";
    case JournalRecordType::ResourceStateChange: return "ResourceStateChange";
    case JournalRecordType::BackendPut: return "BackendPut";
    case JournalRecordType::EpochAdvance: return "EpochAdvance";
    case JournalRecordType::EnvelopeOpened: return "EnvelopeOpened";
    case JournalRecordType::EnvelopeBindingChange: return "EnvelopeBindingChange";
    case JournalRecordType::EnvelopeAuthorized: return "EnvelopeAuthorized";
    case JournalRecordType::EnvelopeStateChange: return "EnvelopeStateChange";
    case JournalRecordType::EnvelopeEffectRecorded: return "EnvelopeEffectRecorded";
    case JournalRecordType::EnvelopeCounters: return "EnvelopeCounters";
    case JournalRecordType::AttemptCreated: return "AttemptCreated";
    case JournalRecordType::AttemptDispatched: return "AttemptDispatched";
    case JournalRecordType::AttemptAcknowledged: return "AttemptAcknowledged";
    case JournalRecordType::AttemptTerminated: return "AttemptTerminated";
    case JournalRecordType::AttemptVerified: return "AttemptVerified";
    case JournalRecordType::AttemptCompensating: return "AttemptCompensating";
    case JournalRecordType::TxnBegin: return "TxnBegin";
    case JournalRecordType::TxnCommit: return "TxnCommit";
    case JournalRecordType::CompactionMarker: return "CompactionMarker";
  }
  return "Unknown";
}

std::string_view to_string_view(JournalStatus status) noexcept {
  switch (status) {
    case JournalStatus::Ok: return "Ok";
    case JournalStatus::NotOpen: return "NotOpen";
    case JournalStatus::IoError: return "IoError";
    case JournalStatus::Corrupt: return "Corrupt";
    case JournalStatus::Truncated: return "Truncated";
    case JournalStatus::VersionUnsupported: return "VersionUnsupported";
    case JournalStatus::BoundsExceeded: return "BoundsExceeded";
    case JournalStatus::NoTransaction: return "NoTransaction";
    case JournalStatus::TransactionOpen: return "TransactionOpen";
    case JournalStatus::PayloadTooLarge: return "PayloadTooLarge";
  }
  return "Unknown";
}

std::vector<std::byte> encode_journal_record(const JournalRecord& record, u64 payload_bound) {
  std::vector<std::byte> out;
  if (record.payload.size() > payload_bound || record.payload.size() > 0xFFFFFFFFULL) {
    return out;
  }
  ByteWriter writer(kJournalRecordHeaderSize + record.payload.size());
  writer.u32(kJournalRecordMagic);
  writer.u16(kJournalFormatVersion);
  writer.u16(static_cast<u16>(record.type));
  writer.u32(0);
  writer.u64(record.sequence);
  writer.u64(record.txn_id);
  writer.u32(static_cast<u32>(record.payload.size()));
  const std::vector<std::byte>& header_body = writer.data();
  u32 crc = crc32c(std::span<const std::byte>(header_body.data(), kHeaderWithoutCrc));
  crc = crc32c_extend(crc, record.payload);
  writer.u32(crc);
  writer.raw(record.payload);
  return writer.data();
}

bool decode_journal_record(std::span<const std::byte> bytes, usize offset, JournalRecord& out,
                           usize& consumed, JournalStatus& status, u64 payload_bound) noexcept {
  consumed = 0;
  status = JournalStatus::Ok;
  if (bytes.size() - offset < kJournalRecordHeaderSize) {
    status = JournalStatus::Truncated;
    return false;
  }
  const std::byte* base = bytes.data() + offset;
  const std::span<const std::byte> header(base, kJournalRecordHeaderSize);
  ByteReader reader(header);
  u32 magic = 0;
  u16 version = 0;
  u16 raw_type = 0;
  u32 flags = 0;
  u64 sequence = 0;
  u64 txn_id = 0;
  u32 payload_len = 0;
  u32 stored_crc = 0;
  if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(raw_type) || !reader.u32(flags) ||
      !reader.u64(sequence) || !reader.u64(txn_id) || !reader.u32(payload_len) ||
      !reader.u32(stored_crc)) {
    status = JournalStatus::Truncated;
    return false;
  }
  if (magic != kJournalRecordMagic) {
    status = JournalStatus::Corrupt;
    return false;
  }
  if (version != kJournalFormatVersion) {
    status = JournalStatus::VersionUnsupported;
    return false;
  }
  if (flags != 0) {
    status = JournalStatus::Corrupt;
    return false;
  }
  if (static_cast<u64>(payload_len) > payload_bound) {
    status = JournalStatus::BoundsExceeded;
    return false;
  }
  const usize total = kJournalRecordHeaderSize + static_cast<usize>(payload_len);
  if (bytes.size() - offset < total) {
    status = JournalStatus::Truncated;
    return false;
  }
  const std::span<const std::byte> payload(base + kJournalRecordHeaderSize,
                                           static_cast<usize>(payload_len));
  u32 crc = crc32c(std::span<const std::byte>(base, kHeaderWithoutCrc));
  crc = crc32c_extend(crc, payload);
  if (crc != stored_crc) {
    status = JournalStatus::Corrupt;
    return false;
  }
  out.type = static_cast<JournalRecordType>(raw_type);
  out.sequence = sequence;
  out.txn_id = txn_id;
  out.payload.assign(payload.begin(), payload.end());
  consumed = total;
  return true;
}

// ---------------------------------------------------------------------------
// Journal writer
// ---------------------------------------------------------------------------
Journal::~Journal() { close(); }

std::unique_ptr<Journal> Journal::open(const JournalConfig& config, bool create_new,
                                       JournalResult& result) {
  result = JournalResult{};
  if (config.path.empty()) {
    result.status = JournalStatus::IoError;
    result.message = "journal path is empty";
    return nullptr;
  }
  const char* mode = create_new ? "wb+" : "ab+";
  std::FILE* file = std::fopen(config.path.c_str(), mode);
  if (file == nullptr) {
    result.status = JournalStatus::IoError;
    result.message = "cannot open journal file";
    return nullptr;
  }
  auto journal = std::unique_ptr<Journal>(new Journal());
  journal->config_ = config;
  journal->file_ = file;
  if (create_new) {
    JournalRecord banner;
    banner.type = JournalRecordType::FileHeader;
    banner.sequence = 0;
    banner.txn_id = 0;
    ByteWriter writer(32);
    writer.u16(kJournalFormatVersion);
    writer.u16(kJournalLayoutRevision);
    std::vector<std::byte> payload = writer.data();
    const JournalResult banner_result = journal->write_record(JournalRecordType::FileHeader, 0,
                                                              payload);
    if (!banner_result) {
      journal->close();
      result = banner_result;
      return nullptr;
    }
    if (!journal->flush()) {
      journal->close();
      result.status = JournalStatus::IoError;
      result.message = "cannot flush journal banner";
      return nullptr;
    }
  } else {
    journal->size_bytes_ = measure_file_size(file);
    if (std::fseek(file, 0, SEEK_END) != 0) {
      journal->close();
      result.status = JournalStatus::IoError;
      result.message = "cannot seek journal to end";
      return nullptr;
    }
  }
  result.ok = true;
  result.sequence = journal->last_sequence_;
  return journal;
}

JournalResult Journal::write_record(JournalRecordType type, u64 txn_id,
                                    std::span<const std::byte> payload) {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (static_cast<u64>(payload.size()) > config_.max_payload_bytes) {
    result.status = JournalStatus::PayloadTooLarge;
    result.message = "journal payload exceeds the configured bound";
    return result;
  }
  JournalRecord record;
  record.type = type;
  record.sequence = last_sequence_ + 1;
  record.txn_id = txn_id;
  record.payload.assign(payload.begin(), payload.end());
  const std::vector<std::byte> encoded = encode_journal_record(record, config_.max_payload_bytes);
  if (encoded.empty()) {
    result.status = JournalStatus::PayloadTooLarge;
    result.message = "journal record could not be encoded within bounds";
    return result;
  }
  if (!write_all(encoded.data(), encoded.size())) {
    result.status = JournalStatus::IoError;
    result.message = "journal write failed";
    return result;
  }
  size_bytes_ += static_cast<u64>(encoded.size());
  last_sequence_ = record.sequence;
  ++record_count_;
  result.ok = true;
  result.sequence = last_sequence_;
  return result;
}

bool Journal::write_all(const void* data, usize size) {
  if (size == 0) {
    return true;
  }
  const auto* bytes = static_cast<const unsigned char*>(data);
  usize written = 0;
  while (written < size) {
    const usize chunk = std::fwrite(bytes + written, 1, size - written, file_);
    if (chunk == 0) {
      return false;
    }
    written += chunk;
  }
  return true;
}

JournalResult Journal::begin(u64 txn_id) {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (txn_open_) {
    result.status = JournalStatus::TransactionOpen;
    result.message = "a journal transaction is already open";
    return result;
  }
  if (txn_id == 0) {
    result.status = JournalStatus::NoTransaction;
    result.message = "transaction id zero is reserved for self-committing records";
    return result;
  }
  ByteWriter writer(8);
  writer.u64(txn_id);
  const std::vector<std::byte>& payload = writer.data();
  result = write_record(JournalRecordType::TxnBegin, txn_id, payload);
  if (!result) {
    return result;
  }
  txn_id_ = txn_id;
  txn_open_ = true;
  txn_record_count_ = 0;
  txn_bytes_ = 0;
  return result;
}

JournalResult Journal::append(JournalRecordType type, std::span<const std::byte> payload) {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (!txn_open_) {
    result.status = JournalStatus::NoTransaction;
    result.message = "append requires an open transaction";
    return result;
  }
  result = write_record(type, txn_id_, payload);
  if (result) {
    ++txn_record_count_;
    txn_bytes_ += payload.size();
  }
  return result;
}

JournalResult Journal::commit() {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (!txn_open_) {
    result.status = JournalStatus::NoTransaction;
    result.message = "commit requires an open transaction";
    return result;
  }
  const std::vector<std::byte> payload = commit_payload(txn_id_, txn_record_count_, txn_bytes_);
  result = write_record(JournalRecordType::TxnCommit, txn_id_, payload);
  if (!result) {
    return result;
  }
  if (config_.fsync_on_commit && !durable_barrier(file_)) {
    result.ok = false;
    result.status = JournalStatus::IoError;
    result.message = "durability barrier failed; the transaction is not durable";
    return result;
  }
  txn_open_ = false;
  txn_id_ = 0;
  txn_record_count_ = 0;
  txn_bytes_ = 0;
  return result;
}

JournalResult Journal::rollback() {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (!txn_open_) {
    result.status = JournalStatus::NoTransaction;
    result.message = "rollback requires an open transaction";
    return result;
  }
  // Records already written stay on disk but carry no commit record, so
  // recovery discards them. Nothing is rewritten in place.
  txn_open_ = false;
  txn_id_ = 0;
  txn_record_count_ = 0;
  txn_bytes_ = 0;
  result.ok = true;
  result.sequence = last_sequence_;
  return result;
}

bool Journal::flush() {
  if (file_ == nullptr) {
    return false;
  }
  return durable_barrier(file_);
}

void Journal::close() noexcept {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

JournalResult Journal::compact_with(std::vector<JournalRecord> snapshot_records) {
  JournalResult result;
  if (file_ == nullptr) {
    result.status = JournalStatus::NotOpen;
    result.message = "journal is not open";
    return result;
  }
  if (txn_open_) {
    result.status = JournalStatus::TransactionOpen;
    result.message = "cannot compact with an open transaction";
    return result;
  }
  const std::string temporary = config_.path + ".compact-tmp";
  std::FILE* target = std::fopen(temporary.c_str(), "wb+");
  if (target == nullptr) {
    result.status = JournalStatus::IoError;
    result.message = "cannot create compaction temporary file";
    return result;
  }

  u64 sequence = 0;
  u64 written_bytes = 0;
  bool ok = true;
  std::string failure;
  auto write_one = [&](JournalRecordType type, u64 txn_id, u64 seq,
                       std::span<const std::byte> payload) {
    JournalRecord record;
    record.type = type;
    record.sequence = seq;
    record.txn_id = txn_id;
    record.payload.assign(payload.begin(), payload.end());
    const std::vector<std::byte> encoded = encode_journal_record(record, config_.max_payload_bytes);
    if (encoded.empty()) {
      ok = false;
      failure = "snapshot record exceeds the payload bound";
      return;
    }
    if (std::fwrite(encoded.data(), 1, encoded.size(), target) != encoded.size()) {
      ok = false;
      failure = "snapshot write failed";
      return;
    }
    written_bytes += static_cast<u64>(encoded.size());
  };

  ByteWriter banner(32);
  banner.u16(kJournalFormatVersion);
  banner.u16(kJournalLayoutRevision);
  write_one(JournalRecordType::FileHeader, 0, 0, banner.data());

  if (ok) {
    ByteWriter begin_payload(8);
    begin_payload.u64(1);
    write_one(JournalRecordType::TxnBegin, 1, 1, begin_payload.data());
  }

  u64 seq = 1;
  u64 payload_bytes = 0;
  for (const JournalRecord& record : snapshot_records) {
    if (!ok) {
      break;
    }
    ++seq;
    payload_bytes += record.payload.size();
    write_one(record.type, 1, seq, record.payload);
  }
  if (ok) {
    ++seq;
    const std::vector<std::byte> commit = commit_payload(1, snapshot_records.size(), payload_bytes);
    write_one(JournalRecordType::TxnCommit, 1, seq, commit);
  }
  sequence = seq;

  if (ok && !durable_barrier(target)) {
    ok = false;
    failure = "compaction durability barrier failed";
  }
  std::fclose(target);

  if (!ok) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    result.status = JournalStatus::IoError;
    result.message = failure.empty() ? "compaction failed" : failure;
    return result;
  }

  std::fclose(file_);
  file_ = nullptr;
  std::error_code error;
  std::filesystem::rename(temporary, config_.path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    // The original log is untouched on disk; reopen it so the engine can keep
    // reporting a precise durability state instead of silently accepting
    // non-durable mutations.
    file_ = std::fopen(config_.path.c_str(), "ab+");
    result.status = JournalStatus::IoError;
    result.message = "compaction rename failed; original log retained";
    return result;
  }

  file_ = std::fopen(config_.path.c_str(), "ab+");
  if (file_ == nullptr) {
    result.status = JournalStatus::IoError;
    result.message = "compacted log could not be reopened";
    return result;
  }
  size_bytes_ = written_bytes;
  last_sequence_ = sequence;
  record_count_ = snapshot_records.size() + 3;
  result.ok = true;
  result.sequence = sequence;
  return result;
}

// ---------------------------------------------------------------------------
// Journal scanner
// ---------------------------------------------------------------------------
JournalScanResult scan_journal_file(const std::string& path) {
  JournalScanResult result;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    result.status = JournalStatus::IoError;
    result.message = "journal file is not readable";
    return result;
  }
  const u64 size = measure_file_size(file);
  if (size > kJournalMaxScanBytes) {
    std::fclose(file);
    result.status = JournalStatus::BoundsExceeded;
    result.message = "journal exceeds the maximum scannable size";
    return result;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    result.status = JournalStatus::IoError;
    result.message = "journal rewind failed";
    return result;
  }
  std::vector<std::byte> buffer(static_cast<usize>(size));
  if (size > 0 && std::fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
    std::fclose(file);
    result.status = JournalStatus::IoError;
    result.message = "journal read failed";
    return result;
  }
  std::fclose(file);

  std::vector<JournalRecord> pending;
  u64 pending_txn = 0;
  usize offset = 0;
  const u64 payload_bound = 4ULL * 1024ULL * 1024ULL;
  while (offset < buffer.size()) {
    JournalRecord record;
    usize consumed = 0;
    JournalStatus status = JournalStatus::Ok;
    const std::span<const std::byte> view(buffer.data(), buffer.size());
    if (!decode_journal_record(view, offset, record, consumed, status, payload_bound)) {
      if (status == JournalStatus::Truncated) {
        result.truncated_tail = true;
        result.discarded_truncated_bytes += static_cast<u64>(buffer.size() - offset);
        result.message = "journal ends with an incomplete record; the tail was discarded";
      } else {
        result.status = status;
        result.message = std::string("journal record rejected: ") +
                         std::string(to_string_view(status));
      }
      break;
    }
    offset += consumed;
    if (record.sequence > result.last_sequence) {
      result.last_sequence = record.sequence;
    }

    if (record.txn_id == 0) {
      if (record.type == JournalRecordType::FileHeader) {
        result.has_file_header = true;
      }
      result.committed.push_back(record);
      continue;
    }
    if (record.type == JournalRecordType::TxnBegin) {
      pending.clear();
      pending_txn = record.txn_id;
      continue;
    }
    if (record.type == JournalRecordType::TxnCommit) {
      if (record.txn_id != pending_txn) {
        result.status = JournalStatus::Corrupt;
        result.message = "commit record does not match the open transaction";
        break;
      }
      for (const JournalRecord& staged : pending) {
        result.committed.push_back(staged);
      }
      ++result.committed_transactions;
      pending.clear();
      pending_txn = 0;
      continue;
    }
    if (pending_txn == 0) {
      // A payload record outside any transaction is discarded rather than
      // guessed at: an unbound mutation is not authoritative evidence.
      ++result.discarded_uncommitted_records;
      continue;
    }
    pending.push_back(record);
  }

  if (!pending.empty()) {
    result.discarded_uncommitted_records += pending.size();
    if (result.message.empty()) {
      result.message = "journal ended with an uncommitted transaction; it was discarded";
    }
    result.truncated_tail = true;
  }
  if (result.status == JournalStatus::Ok || result.status == JournalStatus::Truncated) {
    result.ok = true;
  }
  if (result.message.empty()) {
    result.message = "journal scanned cleanly";
  }
  return result;
}

}  // namespace rate_governor
