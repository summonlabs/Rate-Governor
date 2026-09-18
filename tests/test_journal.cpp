#include "test_support.hpp"

#include <cstdio>
#include <fstream>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

std::vector<std::byte> payload_from(std::string_view text) {
  const auto* first = reinterpret_cast<const std::byte*>(text.data());
  return std::vector<std::byte>(first, first + text.size());
}

JournalRecord make_record(JournalRecordType type, u64 sequence, u64 txn_id,
                          std::string_view text) {
  JournalRecord record;
  record.type = type;
  record.sequence = sequence;
  record.txn_id = txn_id;
  record.payload = payload_from(text);
  return record;
}

u64 file_size(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0ULL : static_cast<u64>(size);
}

void truncate_file(const std::string& path, u64 keep) {
  std::error_code error;
  std::filesystem::resize_file(path, keep, error);
}

void flip_byte(const std::string& path, u64 offset) {
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(offset));
  char value = 0;
  stream.read(&value, 1);
  value = static_cast<char>(value ^ 0x5A);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&value, 1);
}

}  // namespace

RG_TEST(journal, committed_transactions_survive_a_scan) {
  ScratchDir scratch("journal_commit");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);
  RG_CHECK(journal->open());

  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("grant")).ok);
  RG_CHECK(journal->commit().ok);
  RG_CHECK(journal->begin(2).ok);
  RG_CHECK(journal->append(JournalRecordType::PolicyPut, payload_from("policy")).ok);
  RG_CHECK(journal->commit().ok);

  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  RG_CHECK(!scan.truncated_tail);
  RG_CHECK_EQ(scan.committed_transactions, 2ULL);
  RG_CHECK_EQ(scan.discarded_uncommitted_records, 0ULL);
  RG_CHECK(scan.has_file_header);
  // The file banner plus the two committed payload records. Transaction
  // control records are consumed by the scanner and are not evidence.
  RG_CHECK_EQ(scan.committed.size(), 3ULL);
  journal->close();
}

RG_TEST(journal, uncommitted_transactions_are_discarded_not_replayed) {
  ScratchDir scratch("journal_uncommitted");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);

  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("committed")).ok);
  RG_CHECK(journal->commit().ok);

  // A transaction that never commits: the process died mid-write.
  RG_CHECK(journal->begin(2).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("orphan")).ok);
  journal->close();

  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  RG_CHECK_EQ(scan.discarded_uncommitted_records, 1ULL);
  RG_CHECK(scan.truncated_tail);
  for (const JournalRecord& record : scan.committed) {
    const std::string text(reinterpret_cast<const char*>(record.payload.data()),
                           record.payload.size());
    RG_CHECK(text != "orphan");
  }
}

RG_TEST(journal, torn_tail_is_dropped_and_reported) {
  ScratchDir scratch("journal_torn");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);
  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("first")).ok);
  RG_CHECK(journal->commit().ok);
  const u64 intact = file_size(path);
  RG_CHECK(journal->begin(2).ok);
  RG_CHECK(journal->append(JournalRecordType::ResourcePut, payload_from("second")).ok);
  RG_CHECK(journal->commit().ok);
  journal->close();

  // Cut the log in the middle of the second transaction.
  truncate_file(path, intact + 7);
  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  RG_CHECK(scan.truncated_tail);
  RG_CHECK(scan.discarded_truncated_bytes > 0);
  RG_CHECK_EQ(scan.committed_transactions, 1ULL);
  RG_CHECK(!scan.message.empty());
}

RG_TEST(journal, corruption_in_the_body_is_detected) {
  ScratchDir scratch("journal_corrupt");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);
  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("abcdefghij")).ok);
  RG_CHECK(journal->commit().ok);
  journal->close();

  // Flip a bit inside the payload of the first payload record.
  const u64 size = file_size(path);
  RG_CHECK(size > kJournalRecordHeaderSize + 8);
  flip_byte(path, kJournalRecordHeaderSize + 40 + 2);
  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(!scan.ok);
  RG_CHECK(scan.status == JournalStatus::Corrupt);
}

RG_TEST(journal, bad_magic_and_version_are_refused) {
  ScratchDir scratch("journal_magic");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);
  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from("data")).ok);
  RG_CHECK(journal->commit().ok);
  journal->close();

  flip_byte(path, 0);
  JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(!scan.ok);
  RG_CHECK(scan.status == JournalStatus::Corrupt);

  // Rewrite the file from scratch and bump the format version instead.
  std::unique_ptr<Journal> second = Journal::open(config, true, result);
  RG_REQUIRE(second != nullptr);
  RG_CHECK(second->begin(1).ok);
  RG_CHECK(second->append(JournalRecordType::GrantPut, payload_from("data")).ok);
  RG_CHECK(second->commit().ok);
  second->close();
  {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    const char version = 99;
    stream.seekp(4);
    stream.write(&version, 1);
  }
  scan = scan_journal_file(path);
  RG_CHECK(!scan.ok);
  RG_CHECK(scan.status == JournalStatus::VersionUnsupported);
}

RG_TEST(journal, payload_bound_and_transaction_discipline_are_enforced) {
  ScratchDir scratch("journal_bounds");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  config.max_payload_bytes = 64;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);

  // An append without an open transaction is refused.
  const JournalResult stray = journal->append(JournalRecordType::GrantPut, payload_from("x"));
  RG_CHECK(!stray.ok);
  RG_CHECK(stray.status == JournalStatus::NoTransaction);
  RG_CHECK(!journal->commit().ok);

  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(!journal->begin(2).ok);
  const std::vector<std::byte> oversized(128, std::byte{1});
  const JournalResult big = journal->append(JournalRecordType::GrantPut, oversized);
  RG_CHECK(!big.ok);
  RG_CHECK(big.status == JournalStatus::PayloadTooLarge);
  RG_CHECK(journal->rollback().ok);
  RG_CHECK(!journal->commit().ok);
  journal->close();

  // The rolled-back transaction was never committed.
  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  RG_CHECK_EQ(scan.committed_transactions, 0ULL);
}

RG_TEST(journal, compaction_preserves_state_and_bounds_growth) {
  ScratchDir scratch("journal_compact");
  const std::string path = scratch.file("state.rgjournal");
  JournalConfig config;
  config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(config, true, result);
  RG_REQUIRE(journal != nullptr);

  std::vector<JournalRecord> snapshot;
  for (int index = 0; index < 20; ++index) {
    snapshot.push_back(make_record(JournalRecordType::GrantPut, static_cast<u64>(index + 1), 1,
                                   std::string(256, 'g')));
    RG_CHECK(journal->begin(static_cast<u64>(index + 1)).ok);
    RG_CHECK(journal->append(JournalRecordType::GrantPut, payload_from(std::string(256, 'x'))).ok);
    RG_CHECK(journal->commit().ok);
  }
  const u64 before = file_size(path);
  RG_CHECK(before > 0);
  const JournalResult compacted = journal->compact_with(snapshot);
  RG_CHECK(compacted.ok);
  const u64 after = file_size(path);
  RG_CHECK(after < before);
  journal->close();

  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  // The compacted file carries the snapshot plus its own file banner.
  RG_CHECK_EQ(scan.committed.size(), snapshot.size() + 1);
  RG_CHECK_EQ(scan.discarded_uncommitted_records, 0ULL);
  for (const JournalRecord& record : scan.committed) {
    if (record.type == JournalRecordType::FileHeader) {
      continue;
    }
    RG_CHECK_EQ(record.payload.size(), 256ULL);
  }
}

RG_TEST(journal, record_framing_round_trip_is_exact) {
  const JournalRecord record = make_record(JournalRecordType::EnvelopeOpened, 7, 3, "envelope");
  const std::vector<std::byte> encoded = encode_journal_record(record, 1024);
  RG_CHECK(!encoded.empty());
  JournalRecord decoded;
  usize consumed = 0;
  JournalStatus status = JournalStatus::Ok;
  RG_CHECK(decode_journal_record(encoded, 0, decoded, consumed, status, 1024));
  RG_CHECK_EQ(consumed, encoded.size());
  RG_CHECK(decoded.type == record.type);
  RG_CHECK_EQ(decoded.sequence, 7ULL);
  RG_CHECK_EQ(decoded.txn_id, 3ULL);
  RG_CHECK(decoded.payload == record.payload);

  // A payload beyond the bound cannot be encoded at all.
  JournalRecord huge = make_record(JournalRecordType::GrantPut, 1, 1, std::string(2048, 'x'));
  RG_CHECK(encode_journal_record(huge, 128).empty());

  // A record whose declared length exceeds the bound is refused before use.
  std::vector<std::byte> widened = encoded;
  widened[28] = std::byte{0xFF};
  widened[29] = std::byte{0xFF};
  widened[30] = std::byte{0xFF};
  widened[31] = std::byte{0x7F};
  RG_CHECK(!decode_journal_record(widened, 0, decoded, consumed, status, 1024));
  RG_CHECK(status == JournalStatus::BoundsExceeded);
}

RG_TEST(journal, engine_mutations_are_durable_and_replayable) {
  ScratchDir scratch("journal_engine");
  const std::string path = scratch.file("engine.rgjournal");
  JournalConfig journal_config;
  journal_config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(journal_config, true, result);
  RG_REQUIRE(journal != nullptr);

  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::Strict;
  config.boot = WorkerBootId(1);
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           config.boot, config.initial_epoch, "journal test");
  RateGovernor engine(config, clock, &backend, journal.get(), nullptr);
  const FixtureIds ids = install_fixture(engine, FixtureOptions{});
  BackendDescriptor descriptor = backend.describe();
  RG_REQUIRE(engine.put_backend(descriptor).has_value());
  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "journal test");
  const Result<RateEnvelopeId> opened =
      engine.open_envelope(make_request(ids, FixtureOptions{}), actor);
  RG_REQUIRE(opened.has_value());

  // Everything the engine acknowledged must already be on disk.
  const u64 size_after_open = journal->size_bytes();
  RG_CHECK(size_after_open > 0);
  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  RG_CHECK_EQ(scan.discarded_uncommitted_records, 0ULL);
  RG_CHECK(scan.committed_transactions > 0);

  // The snapshot the engine publishes reproduces its own state exactly.
  const std::vector<JournalRecord> snapshot = engine.snapshot_records();
  RG_CHECK(!snapshot.empty());
  ManualClock replay_clock(0);
  EngineConfig replay_config;
  replay_config.durability = DurabilityMode::None;
  RateGovernor replay(replay_config, replay_clock, nullptr, nullptr, nullptr);
  replay.apply_snapshot_records(snapshot);
  const EngineStorageStats original = engine.storage_stats();
  const EngineStorageStats recovered = replay.storage_stats();
  RG_CHECK_EQ(original.envelopes, recovered.envelopes);
  RG_CHECK_EQ(original.grants, recovered.grants);
  RG_CHECK_EQ(original.policies, recovered.policies);
  RG_CHECK_EQ(original.resources, recovered.resources);
  RG_CHECK_EQ(recovered.recovery_orphan_records, 0ULL);
  const Result<EnvelopeView> original_view = engine.inspect(opened.value());
  const Result<EnvelopeView> replay_view = replay.inspect(opened.value());
  RG_REQUIRE(original_view.has_value() && replay_view.has_value());
  RG_CHECK(original_view.value().state == replay_view.value().state);
  RG_CHECK_EQ(original_view.value().effective_ceiling_ups, replay_view.value().effective_ceiling_ups);
  RG_CHECK_EQ(original_view.value().effective_burst_tokens, replay_view.value().effective_burst_tokens);
  journal->close();
}

RG_TEST(journal, orphan_patch_records_are_reported_not_guessed) {
  ScratchDir scratch("journal_orphan");
  const std::string path = scratch.file("engine.rgjournal");
  JournalConfig journal_config;
  journal_config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(journal_config, true, result);
  RG_REQUIRE(journal != nullptr);
  RG_CHECK(journal->begin(1).ok);
  RG_CHECK(journal->append(JournalRecordType::GrantStateChange, payload_from("not a grant")).ok);
  RG_CHECK(journal->commit().ok);
  journal->close();

  const JournalScanResult scan = scan_journal_file(path);
  RG_CHECK(scan.ok);
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  RateGovernor engine(config, clock, nullptr, nullptr, nullptr);
  engine.apply_snapshot_records(scan.committed);
  RG_CHECK_EQ(engine.storage_stats().recovery_orphan_records, 1ULL);
  RG_CHECK_EQ(engine.storage_stats().grants, 0ULL);
}
