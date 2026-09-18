#include "test_support.hpp"

#include <fstream>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

// A backend that performs the effect but withholds the completion, which is
// what leaves an attempt in flight at the moment of a crash.
class DeferringBackend final : public IEnforcementBackend {
 public:
  DeferringBackend(WorkerBootId boot, FabricEpoch epoch)
      : device_(SyntheticDeviceConfig{}, boot, epoch, boot) {
    descriptor_.id = BackendId(1);
    descriptor_.generation = Generation(1);
    descriptor_.kind = BackendKind::Synthetic;
    descriptor_.capabilities =
        kBackendCapApply | kBackendCapRevoke | kBackendCapVerify | kBackendCapBurst;
    descriptor_.verification = VerificationMode::PostApplyReadback;
    descriptor_.label = "deferring test device";
    descriptor_.provenance.source = AuthoritySource::Configuration;
    descriptor_.provenance.detail = "recovery test backend";
  }

  [[nodiscard]] BackendDescriptor describe() const override { return descriptor_; }

  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override {
    (void)device_.apply(request);
    DispatchOutcome outcome;
    outcome.status = DispatchStatus::Pending;
    outcome.reason = ReasonCode::ApplyDispatched;
    outcome.detail = "completion withheld";
    outcome.epoch = device_.highest_epoch();
    outcome.boot = device_.session_boot();
    return outcome;
  }
  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override {
    return device_.revoke(request);
  }
  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override {
    return device_.read_back(request);
  }
  void shutdown() noexcept override {}
  [[nodiscard]] WorkerBootId session_boot() const override { return device_.session_boot(); }
  [[nodiscard]] FabricEpoch session_epoch() const override { return device_.highest_epoch(); }
  [[nodiscard]] bool session_live() const override { return true; }
  [[nodiscard]] std::string session_description() const override { return "deferring synthetic"; }

 private:
  SyntheticDevice device_;
  BackendDescriptor descriptor_{};
};

struct RecoveredFixture {
  std::unique_ptr<Journal> journal;
  std::unique_ptr<RateGovernor> engine;
  RecoveryReport report;
  bool recovered{false};
};

// Creates a journal, runs a scenario in it, and returns the durable path.
std::string seed_journal(const std::string& path, bool apply_and_verify) {
  JournalConfig journal_config;
  journal_config.path = path;
  JournalResult result;
  std::unique_ptr<Journal> journal = Journal::open(journal_config, true, result);
  if (journal == nullptr) {
    return {};
  }
  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::Strict;
  config.boot = WorkerBootId(5);
  SyntheticDeviceConfig device_config;
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), device_config, config.boot,
                           config.initial_epoch, "seed backend");
  RateGovernor engine(config, clock, &backend, journal.get(), nullptr);
  const FixtureIds ids = install_fixture(engine, FixtureOptions{});
  BackendDescriptor descriptor = backend.describe();
  (void)engine.put_backend(descriptor);
  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "seed actor");
  const Result<RateEnvelopeId> opened =
      engine.open_envelope(make_request(ids, FixtureOptions{}), actor);
  if (!opened) {
    journal->close();
    return {};
  }
  if (apply_and_verify) {
    const Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
    if (!applied || !applied.value().verified) {
      journal->close();
      return {};
    }
  }
  journal->close();
  return path;
}

RecoveredFixture recover(const std::string& path, bool truncate_tail = true) {
  RecoveredFixture fixture;
  EngineConfig config;
  config.boot = WorkerBootId(77);
  config.journal.path = path;
  static ManualClock recovery_clock(0);
  Result<RecoveredEngine> recovered =
      recover_engine(path, config, recovery_clock, nullptr, truncate_tail);
  if (!recovered) {
    return fixture;
  }
  fixture.report = recovered.value().report;
  fixture.journal = std::move(recovered.value().journal);
  fixture.engine = std::move(recovered.value().engine);
  fixture.recovered = true;
  return fixture;
}

}  // namespace

RG_TEST(recovery, a_missing_journal_is_refused_rather_than_invented) {
  ScratchDir scratch("recovery_missing");
  const std::string path = scratch.file("absent.rgjournal");
  EngineConfig config;
  config.boot = WorkerBootId(1);
  ManualClock clock(0);
  const Result<RecoveredEngine> recovered = recover_engine(path, config, clock, nullptr, false);
  RG_CHECK(!recovered.has_value());
  RG_CHECK(recovered.code() == ErrorCode::JournalFailure);
  RG_CHECK(!std::filesystem::exists(path));
}

RG_TEST(recovery, durable_state_is_restored_but_effect_is_demoted) {
  ScratchDir scratch("recovery_demote");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());

  RecoveredFixture fixture = recover(path);
  RG_REQUIRE(fixture.recovered);
  RG_CHECK_EQ(fixture.report.envelopes, 1ULL);
  RG_CHECK_EQ(fixture.report.grants, 1ULL);
  RG_CHECK_EQ(fixture.report.policies, 1ULL);
  RG_CHECK_EQ(fixture.report.effect_claims_demoted, 1ULL);
  RG_CHECK_EQ(fixture.report.envelopes_requiring_revalidation, 1ULL);
  RG_CHECK(!fixture.report.liveness_restored);
  RG_CHECK(!fixture.report.backend_bound);
  RG_CHECK_EQ(fixture.report.previous_boot.value(), 5ULL);
  RG_CHECK_EQ(fixture.report.new_boot.value(), 77ULL);
  RG_CHECK(fixture.report.new_epoch > fixture.report.recovered_epoch);

  const std::vector<EnvelopeView> views = fixture.engine->list_envelopes(8);
  RG_REQUIRE(views.size() == 1);
  const EnvelopeView& view = views.front();
  RG_CHECK(view.state == EnvelopeState::Degraded);
  RG_CHECK(view.reason == ReasonCode::EnvelopeRecoveredRequiresRevalidation);
  RG_CHECK(view.requires_revalidation);
  RG_CHECK(!view.effect_verified);
  RG_CHECK(!view.legally_enforceable_now);
  RG_CHECK(view.action == EnvelopeAction::Revalidate);
  RG_CHECK_EQ(view.effective_ceiling_ups, 1000ULL);

  // The epoch advanced, so the previous incarnation's attempts are fenced.
  const ActorContext stale = operator_context(fixture.report.recovered_epoch, WorkerBootId(5),
                                              "stale actor");
  RG_CHECK(!fixture.engine->authorize(RateEnvelopeId(1), stale).has_value());
}

RG_TEST(recovery, unfinished_attempts_become_ambiguous_and_cannot_succeed) {
  ScratchDir scratch("recovery_ambiguous");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, false).empty());

  // Dispatch an attempt without completing it, then crash.
  {
    JournalConfig journal_config;
    journal_config.path = path;
    JournalResult result;
    std::unique_ptr<Journal> journal = Journal::open(journal_config, false, result);
    RG_REQUIRE(journal != nullptr);
    ManualClock clock(0);
    EngineConfig config;
    config.durability = DurabilityMode::Strict;
    config.boot = WorkerBootId(6);
    Result<RecoveredEngine> interim =
        recover_engine(path, config, clock, nullptr, true);
    RG_REQUIRE(interim.has_value());
    std::unique_ptr<RateGovernor> engine = std::move(interim.value().engine);
    auto journal_owner = std::move(interim.value().journal);
    const ActorContext actor = operator_context(engine->epoch(), engine->boot(), "interim actor");
    DeferringBackend backend(engine->boot(), engine->epoch());
    BackendDescriptor descriptor = backend.describe();
    (void)engine->put_backend(descriptor);
    engine->bind_backend(&backend, engine->epoch());
    const Result<RateEnvelopeId> reopened =
        engine->open_envelope(make_request(FixtureIds{}, FixtureOptions{}), actor);
    RG_REQUIRE(reopened.has_value());
    // The completion is withheld, so the attempt is in flight when the process
    // "dies" (the journal is closed without a completion record).
    const Result<ApplyDispatch> dispatch = engine->apply(reopened.value(), actor);
    RG_REQUIRE(dispatch.has_value());
    RG_CHECK(dispatch.value().awaiting_completion);
    RG_CHECK(dispatch.value().resulting_state == EnvelopeState::Dispatching);
    journal_owner->close();
  }

  RecoveredFixture fixture = recover(path);
  RG_REQUIRE(fixture.recovered);
  RG_CHECK(fixture.report.attempts_marked_ambiguous >= 1);
  for (const EnforcementAttempt& attempt :
       fixture.engine->list_attempts(RateEnvelopeId(1), 16)) {
    RG_CHECK(!attempt_state_may_publish_success(attempt.state));
  }
  const std::vector<EnvelopeView> views = fixture.engine->list_envelopes(8);
  RG_REQUIRE(!views.empty());
  RG_CHECK(!views.front().effect_verified);
  RG_CHECK(!views.front().legally_enforceable_now);
}

RG_TEST(recovery, revalidation_without_a_backend_stays_unknown) {
  ScratchDir scratch("recovery_nobackend");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());
  RecoveredFixture fixture = recover(path);
  RG_REQUIRE(fixture.recovered);
  const ActorContext actor = operator_context(fixture.engine->epoch(), fixture.engine->boot(),
                                              "recovery actor");
  const RevalidationReport report = fixture.engine->revalidate_all(actor);
  RG_CHECK_EQ(report.confirmed_applied, 0ULL);
  RG_CHECK_EQ(report.requeued, 0ULL);
  RG_CHECK(report.skipped_no_backend >= 1);
  const std::vector<EnvelopeView> views = fixture.engine->list_envelopes(8);
  RG_REQUIRE(!views.empty());
  RG_CHECK(views.front().requires_revalidation);
  RG_CHECK(!views.front().effect_verified);
}

RG_TEST(recovery, revalidation_denies_a_lost_effect_and_permits_a_fresh_apply) {
  ScratchDir scratch("recovery_revalidate");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());
  RecoveredFixture fixture = recover(path);
  RG_REQUIRE(fixture.recovered);

  // A brand-new worker incarnation holds nothing: the durable claim is not
  // current effect, and revalidation must say so.
  ManualClock clock(0);
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           fixture.engine->boot(), fixture.engine->epoch(), "fresh backend");
  fixture.engine->bind_backend(&backend, fixture.engine->epoch());
  const ActorContext actor = operator_context(fixture.engine->epoch(), fixture.engine->boot(),
                                              "recovery actor");
  const RevalidationReport report = fixture.engine->revalidate_all(actor);
  RG_CHECK_EQ(report.considered, 1ULL);
  RG_CHECK_EQ(report.requeued, 1ULL);
  RG_CHECK_EQ(report.confirmed_applied, 0ULL);
  RG_REQUIRE(report.items.size() == 1);
  RG_CHECK(report.items.front().reapply_required);

  const Result<ApplyDispatch> reapplied = fixture.engine->apply(RateEnvelopeId(1), actor);
  RG_REQUIRE(reapplied.has_value());
  RG_CHECK(reapplied.value().verified);
  const Result<EnvelopeView> view = fixture.engine->inspect(RateEnvelopeId(1));
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Applied);
  RG_CHECK(view.value().legally_enforceable_now);
}

RG_TEST(recovery, revalidation_confirms_an_effect_that_is_still_held) {
  ScratchDir scratch("recovery_confirm");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());

  // Recover, then hold the *same* device incarnation across the boundary by
  // applying the effect again through the recovered engine and recovering once
  // more with the device still live.
  RecoveredFixture first = recover(path);
  RG_REQUIRE(first.recovered);
  ManualClock clock(0);
  SyntheticBackend backend(FixtureIds{}.backend, Generation(1), SyntheticDeviceConfig{},
                           first.engine->boot(), first.engine->epoch(), "persistent backend");
  first.engine->bind_backend(&backend, first.engine->epoch());
  const ActorContext actor = operator_context(first.engine->epoch(), first.engine->boot(),
                                              "recovery actor");
  RG_REQUIRE(first.engine->revalidate_all(actor).requeued == 1);
  RG_REQUIRE(first.engine->apply(RateEnvelopeId(1), actor).has_value());

  // Now demote the recovered engine's own claim without restarting the device.
  u64 attempts_marked = 0;
  u64 envelopes_demoted = 0;
  u64 effects_demoted = 0;
  const Status demoted =
      first.engine->demote_for_recovery(actor, attempts_marked, envelopes_demoted, effects_demoted);
  RG_REQUIRE(demoted.has_value());
  RG_CHECK_EQ(effects_demoted, 1ULL);
  const RevalidationReport confirmed = first.engine->revalidate_all(actor);
  RG_CHECK_EQ(confirmed.confirmed_applied, 1ULL);
  const Result<EnvelopeView> view = first.engine->inspect(RateEnvelopeId(1));
  RG_REQUIRE(view.has_value());
  RG_CHECK(view.value().state == EnvelopeState::Applied);
  RG_CHECK(view.value().reason == ReasonCode::EnvelopeRevalidatedApplied);
  RG_CHECK(view.value().effect_verified);
}

RG_TEST(recovery, truncated_tail_is_truncated_on_request) {
  ScratchDir scratch("recovery_truncate");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());
  const u64 size = std::filesystem::file_size(path);
  RG_REQUIRE(size > 64);

  // Simulate a torn write: keep the records but lose the last bytes.
  std::error_code error;
  std::filesystem::resize_file(path, size - 11, error);
  RG_REQUIRE(!error);

  RecoveredFixture fixture = recover(path, true);
  RG_REQUIRE(fixture.recovered);
  RG_CHECK(fixture.report.truncated_tail);
  RG_CHECK(fixture.report.discarded_truncated_bytes > 0);
  const u64 after = std::filesystem::file_size(path);
  RG_CHECK(after < size);
  const JournalScanResult rescan = scan_journal_file(path);
  RG_CHECK(rescan.ok);
  RG_CHECK(!rescan.truncated_tail);
  RG_CHECK_EQ(rescan.discarded_uncommitted_records, 0ULL);
}

RG_TEST(recovery, repeated_recovery_never_escalates_a_claim_to_truth) {
  ScratchDir scratch("recovery_repeat");
  const std::string path = scratch.file("state.rgjournal");
  RG_REQUIRE(!seed_journal(path, true).empty());
  u64 previous_epoch = 0;
  for (int round = 0; round < 3; ++round) {
    RecoveredFixture fixture = recover(path);
    RG_REQUIRE(fixture.recovered);
    RG_CHECK(fixture.report.new_epoch > fixture.report.recovered_epoch);
    RG_CHECK(fixture.report.new_epoch.value() > previous_epoch);
    previous_epoch = fixture.report.new_epoch.value();
    const std::vector<EnvelopeView> views = fixture.engine->list_envelopes(8);
    RG_REQUIRE(!views.empty());
    RG_CHECK(!views.front().legally_enforceable_now);
    RG_CHECK(!views.front().effect_verified);
    fixture.journal->close();
  }
}
