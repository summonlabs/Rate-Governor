#ifndef RATE_GOVERNOR_ENGINE_HPP
#define RATE_GOVERNOR_ENGINE_HPP

// Rate Governor control plane.
//
// The engine answers, deterministically and explainably:
//   * what rate envelope is legally enforceable right now,
//   * how much burst remains,
//   * what effect has actually been applied (never inferred from an
//     acknowledgement alone),
//   * and when the envelope must be reduced, revoked, revalidated, fenced or
//     rejected as stale.
//
// Locking discipline (audited explicitly in the validation suite):
//   * one state mutex guards all authoritative state;
//   * backend calls, event emission and journal compaction-by-copy happen
//     outside the state mutex; dispatch is a two-phase
//     snapshot -> call -> revalidate -> commit sequence;
//   * no callback is ever invoked while the state mutex is held, so a sink may
//     re-enter the engine for read-only queries without deadlocking;
//   * the engine owns no threads, so shutdown can never deadlock against a
//     worker it is waiting for.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rate_governor/backend.hpp"
#include "rate_governor/clock.hpp"
#include "rate_governor/entities.hpp"
#include "rate_governor/envelope.hpp"
#include "rate_governor/error.hpp"
#include "rate_governor/journal.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

// Defined in plan.hpp, which depends on this header. Only ever returned by
// value from declarations here, never defined inline.
struct PlanInputs;

enum class DurabilityMode : std::uint8_t {
  // No durable log. Intended for pure evaluation, replay and benchmarking.
  // Every mutating call still validates and still refuses stale work; it simply
  // makes no durability claim.
  None = 0,
  // Journal + barrier before acknowledgement.
  Strict,
};

struct EngineConfig {
  FabricEpoch initial_epoch{FabricEpoch(1)};
  WorkerBootId boot{WorkerBootId(1)};
  // A recovery always advances the epoch: an incarnation change must fence
  // everything the previous incarnation could have left in force.
  bool advance_epoch_on_recovery{true};
  DurabilityMode durability{DurabilityMode::Strict};

  u64 max_envelopes{4096};
  u64 max_flows{4096};
  u64 max_grants{4096};
  u64 max_reservations{4096};
  u64 max_policies{4096};
  u64 max_resources{4096};
  u64 max_backends{64};
  u64 max_attempts_per_envelope{32};
  u64 max_audit_events{4096};
  u64 max_attribute_bytes{256};

  // A dispatched attempt that is not completed within this window becomes
  // AMBIGUOUS: it can never publish success afterwards. Zero disables it.
  u64 attempt_deadline_ns{0};
  // Scheduled revalidation horizon recorded on applied envelopes. Zero disables.
  u64 revalidate_interval_ns{0};
  // Note: there is deliberately no "skip verification" switch. An envelope is
  // published as APPLIED only after a post-apply readback confirms the exact
  // requested envelope; an acknowledgement alone never becomes effect.

  // Used only by recovery, which opens the durable log itself. An embedder that
  // constructs an engine directly supplies an already-open Journal instead.
  JournalConfig journal{};
};

struct AuditEvent {
  AuditSequence sequence{};
  TimestampNs at_ns{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  ReasonCode reason{ReasonCode::None};
  ReasonClass klass{ReasonClass::None};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  EnforcementAttemptId attempt{};
  std::string detail;
};

class IEventSink {
 public:
  IEventSink() = default;
  virtual ~IEventSink();
  IEventSink(const IEventSink&) = delete;
  IEventSink& operator=(const IEventSink&) = delete;
  // Called with no engine lock held.
  virtual void on_event(const AuditEvent& event) = 0;
};

struct EngineCounters {
  u64 envelopes_opened{0};
  u64 plans_authorized{0};
  u64 plans_denied{0};
  u64 applies_dispatched{0};
  u64 applies_verified{0};
  u64 applies_degraded{0};
  u64 revokes_dispatched{0};
  u64 revokes_verified{0};
  u64 completions_accepted{0};
  u64 completions_rejected{0};
  u64 completions_duplicate{0};
  u64 stale_transitions{0};
  u64 fencing_rejections{0};
  u64 idempotent_replays{0};
  u64 journal_commits{0};
  u64 journal_failures{0};
  u64 compactions{0};
  u64 events_emitted{0};
  u64 events_dropped{0};
  u64 work_rejected_shutdown{0};
  u64 clock_regressions{0};
  u64 token_consumptions{0};
  u64 token_starvations{0};
};

struct TickReport {
  TimestampNs now_ns{0};
  u64 envelopes_refilled{0};
  u64 envelopes_stale{0};
  u64 envelopes_reduced{0};
  u64 attempts_expired{0};
  u64 revokes_pending{0};
  u64 cooldowns_expired{0};
  u64 clock_regressions{0};
  u64 token_buckets_saturated{0};
  u64 revalidations_required{0};
  u64 capacity_rejections{0};
  // Envelope transitions that exceeded the bounded per-tick budget and will be
  // processed by the next tick.
  u64 deferred{0};
  bool journal_failed{false};
};

struct PlanOutcome {
  bool authorized{false};
  ReasonCode reason{ReasonCode::None};
  EffectivePlan plan{};
  std::string explanation;
};

struct ApplyDispatch {
  EnforcementAttemptId attempt{};
  bool duplicate{false};
  bool dispatched{false};
  bool awaiting_completion{false};
  bool verification_performed{false};
  bool verified{false};
  // The readback showed more than was authorized (or less than the service
  // floor), so a compensating revoke is required.
  bool compensating_revoke_required{false};
  u64 observed_rate_ups{0};
  u64 observed_burst_tokens{0};
  EnvelopeState resulting_state{EnvelopeState::Unknown};
  ReasonCode reason{ReasonCode::None};
  std::string explanation;
};

struct CompletionReport {
  DispatchStatus status{DispatchStatus::Unknown};
  bool acknowledged{false};
  u64 acknowledged_rate_ups{0};
  u64 acknowledged_burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
  TimestampNs received_ns{0};
};

struct CompletionOutcome {
  EnforcementAttemptId attempt{};
  bool accepted{false};
  bool duplicate{false};
  bool rejected_late{false};
  bool verification_performed{false};
  bool verified{false};
  bool disputed{false};
  bool compensating_revoke_required{false};
  ReasonCode reason{ReasonCode::None};
  EnvelopeState resulting_state{EnvelopeState::Unknown};
  std::string explanation;
};

struct RevokeDispatch {
  EnforcementAttemptId attempt{};
  bool duplicate{false};
  bool dispatched{false};
  bool verified{false};
  EnvelopeState resulting_state{EnvelopeState::Unknown};
  ReasonCode reason{ReasonCode::None};
  std::string explanation;
};

struct RevalidationItem {
  RateEnvelopeId envelope{};
  Generation generation{};
  EnvelopeState before{EnvelopeState::Unknown};
  EnvelopeState after{EnvelopeState::Unknown};
  ReasonCode reason{ReasonCode::None};
  bool effect_confirmed{false};
  bool reapply_required{false};
  std::string explanation;
};

struct RevalidationReport {
  u64 considered{0};
  u64 confirmed_applied{0};
  u64 requeued{0};
  u64 unknown{0};
  u64 denied{0};
  u64 skipped_no_backend{0};
  std::vector<RevalidationItem> items;
  std::string explanation;
};

struct ShutdownReport {
  bool work_stopped{false};
  u64 attempts_abandoned{0};
  u64 envelopes_requiring_revalidation{0};
  u64 events_dropped{0};
  bool backend_shutdown{false};
  std::string explanation;
};

// The complete answer to the core question for one envelope.
struct EnvelopeView {
  RateEnvelopeId id{};
  Generation generation{};
  EnvelopeState state{EnvelopeState::Unknown};
  ReasonCode reason{ReasonCode::None};
  EnvelopeAction action{EnvelopeAction::None};
  ReasonCode action_reason{ReasonCode::None};

  bool legally_enforceable_now{false};
  bool has_plan{false};
  u64 effective_ceiling_ups{0};
  u64 effective_floor_ups{0};
  u64 effective_target_ups{0};
  u64 effective_burst_tokens{0};
  u64 burst_tokens_available{0};
  u64 burst_capacity{0};
  LimitingAuthority limiting_authority{LimitingAuthority::None};

  u64 applied_rate_ups{0};
  u64 applied_burst_tokens{0};
  bool effect_verified{false};
  FabricEpoch applied_epoch{};
  WorkerBootId applied_boot{};

  TimestampNs plan_valid_from_ns{0};
  TimestampNs plan_valid_until_ns{0};
  TimestampNs cooldown_until_ns{0};
  TimestampNs revalidate_by_ns{0};
  bool requires_revalidation{false};

  u64 attempts_issued{0};
  u64 completions_applied{0};
  u64 completions_rejected{0};
  std::string annotation;
  std::string explanation;
};

struct EngineStorageStats {
  u64 grants{0};
  u64 reservations{0};
  u64 policies{0};
  u64 resources{0};
  u64 backends{0};
  u64 envelopes{0};
  u64 attempts{0};
  u64 audit_events{0};
  u64 audit_events_dropped{0};
  u64 journal_bytes{0};
  // Records whose target object was absent during replay. A non-zero value
  // means the durable log is structurally inconsistent and was not guessed at.
  u64 recovery_orphan_records{0};
};

class RateGovernor {
 public:
  RateGovernor(EngineConfig config, IClock& clock, IEnforcementBackend* backend, Journal* journal,
               IEventSink* sink);
  ~RateGovernor();
  RateGovernor(const RateGovernor&) = delete;
  RateGovernor& operator=(const RateGovernor&) = delete;

  // ---------------------------------------------------------------------
  // Authority ingest. Each put replaces the current definition only if the
  // supplied generation is strictly newer; a stale or unknown generation is
  // refused so an old controller cannot resurrect superseded authority.
  // ---------------------------------------------------------------------
  [[nodiscard]] Status put_flow(const Flow& flow);
  [[nodiscard]] Status put_grant(const Grant& grant);
  [[nodiscard]] Status put_reservation(const Reservation& reservation);
  [[nodiscard]] Status put_policy(const Policy& policy);
  [[nodiscard]] Status put_resource(const Resource& resource);
  [[nodiscard]] Status put_backend(const BackendDescriptor& backend);

  [[nodiscard]] Status change_grant_state(GrantId id, Generation generation, GrantState state,
                                          ReasonCode reason, const ActorContext& actor);
  [[nodiscard]] Status change_reservation_state(ReservationId id, Generation generation,
                                                ReservationState state, ReasonCode reason,
                                                const ActorContext& actor);
  [[nodiscard]] Status change_policy_state(PolicyId id, Generation generation, PolicyState state,
                                           ReasonCode reason, const ActorContext& actor);
  [[nodiscard]] Status change_resource_state(ResourceId id, Generation generation,
                                             ResourceState state, ReasonCode reason,
                                             const ActorContext& actor);

  // Advances the fabric epoch. Every attempt and plan stamped with an older
  // epoch is fenced from that instant; envelopes that were holding an effect
  // require revalidation because the epoch change invalidates prior effect.
  [[nodiscard]] Status advance_epoch(FabricEpoch new_epoch, const ActorContext& actor);
  // Rebinds the enforcement backend (after a restart or a worker reconnect).
  void bind_backend(IEnforcementBackend* backend, FabricEpoch session_epoch);

  // ---------------------------------------------------------------------
  // Envelope lifecycle
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<RateEnvelopeId> open_envelope(const EnvelopeRequest& request,
                                                     const ActorContext& actor);
  [[nodiscard]] Result<PlanOutcome> authorize(RateEnvelopeId id, const ActorContext& actor);
  // Pure evaluation: derives the plan without mutating any state.
  [[nodiscard]] Result<PlanOutcome> explain_plan(RateEnvelopeId id) const;

  [[nodiscard]] Result<ApplyDispatch> apply(RateEnvelopeId id, const ActorContext& actor);
  [[nodiscard]] Result<CompletionOutcome> complete_apply(EnforcementAttemptId attempt,
                                                         const CompletionReport& report,
                                                         const ActorContext& actor);
  [[nodiscard]] Result<RevokeDispatch> revoke(RateEnvelopeId id, ReasonCode reason,
                                              const ActorContext& actor);
  [[nodiscard]] Result<CompletionOutcome> complete_revoke(EnforcementAttemptId attempt,
                                                          const CompletionReport& report,
                                                          const ActorContext& actor);

  [[nodiscard]] Status cancel_attempt(EnforcementAttemptId attempt, ReasonCode reason,
                                      const ActorContext& actor);
  [[nodiscard]] Status abandon_attempt(EnforcementAttemptId attempt, ReasonCode reason,
                                       const ActorContext& actor);

  // Consumes burst tokens through the envelope's bucket. Returns false and a
  // starvation reason when the bucket cannot cover the request; the balance can
  // never go negative.
  [[nodiscard]] Result<u64> consume_burst(RateEnvelopeId id, u64 tokens);

  [[nodiscard]] TickReport tick();
  [[nodiscard]] RevalidationReport revalidate_all(const ActorContext& actor);
  // Records that the enforcement session is gone. The engine withdraws its own
  // admission of the session and every effect claim established through it is
  // demoted to "requires revalidation": a dead session is not evidence.
  [[nodiscard]] Status mark_session_lost(const ActorContext& actor);
  [[nodiscard]] ShutdownReport shutdown();

  // ---------------------------------------------------------------------
  // Observation
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<EnvelopeView> inspect(RateEnvelopeId id) const;
  [[nodiscard]] std::vector<EnvelopeView> list_envelopes(usize limit) const;
  [[nodiscard]] Result<EnforcementAttempt> inspect_attempt(EnforcementAttemptId id) const;
  [[nodiscard]] std::vector<EnforcementAttempt> list_attempts(RateEnvelopeId id,
                                                              usize limit) const;
  [[nodiscard]] std::vector<AuditEvent> drain_events(usize limit);
  [[nodiscard]] EngineCounters counters() const;
  [[nodiscard]] EngineStorageStats storage_stats() const;
  [[nodiscard]] FabricEpoch epoch() const;
  [[nodiscard]] WorkerBootId boot() const noexcept { return config_.boot; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }
  [[nodiscard]] bool shutting_down() const noexcept {
    return shutting_down_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool durability_degraded() const;

  // Serialises the current authoritative state as journal records. Used by
  // compaction and by the validation suite to prove snapshot equivalence.
  [[nodiscard]] std::vector<JournalRecord> snapshot_records() const;

  // Applies a record produced by snapshot_records(). Only used by recovery.
  void apply_snapshot_records(const std::vector<JournalRecord>& records);

  // Recovery-only: demotes every durable live claim so that nothing on disk is
  // treated as current evidence. Unfinished attempts become AMBIGUOUS; every
  // effect claim is marked as requiring revalidation. This never restores
  // process liveness, worker sessions, leases or publisher authority.
  [[nodiscard]] Status demote_for_recovery(const ActorContext& actor, u64& attempts_marked,
                                           u64& envelopes_demoted, u64& effects_demoted);

 private:
  using StagedRecords = std::vector<std::pair<JournalRecordType, std::vector<std::byte>>>;

  [[nodiscard]] PlanInputs make_plan_inputs_locked(const RateEnvelope& envelope) const;
  [[nodiscard]] PlanOutcome derive_plan_locked(const RateEnvelope& envelope, TimestampNs now) const;
  // ReasonCode::None when every authority the envelope was bound to is still
  // the current authority and still valid at "now".
  [[nodiscard]] ReasonCode authority_status_locked(const RateEnvelope& envelope,
                                                   TimestampNs now) const;
  [[nodiscard]] EnvelopeView make_view_locked(const RateEnvelope& envelope, TimestampNs now) const;
  [[nodiscard]] Status commit_records_locked(const StagedRecords& records);
  [[nodiscard]] Status commit_one_locked(JournalRecordType type, std::vector<std::byte> payload);
  void stage_envelope(StagedRecords& records, const RateEnvelope& envelope,
                      JournalRecordType type) const;
  void stage_attempt(StagedRecords& records, const EnforcementAttempt& attempt,
                     JournalRecordType type) const;
  [[nodiscard]] ApplyRequest make_apply_request(const RateEnvelope& envelope,
                                                const EnforcementAttempt& attempt) const;
  [[nodiscard]] RevokeRequest make_revoke_request(const RateEnvelope& envelope,
                                                  const EnforcementAttempt& attempt,
                                                  ReasonCode reason) const;
  [[nodiscard]] Result<CompletionOutcome> finish_apply(RateEnvelopeId envelope,
                                                       EnforcementAttemptId attempt,
                                                       const ApplyRequest& request,
                                                       const CompletionReport& report);
  [[nodiscard]] Result<CompletionOutcome> finish_revoke(RateEnvelopeId envelope,
                                                        EnforcementAttemptId attempt,
                                                        const RevokeRequest& request,
                                                        const CompletionReport& report);
  void push_event_locked(ReasonCode reason, RateEnvelopeId envelope, Generation generation,
                         EnforcementAttemptId attempt, std::string detail,
                         std::vector<AuditEvent>& out);
  void emit(const std::vector<AuditEvent>& events);
  void maybe_compact_locked();
  [[nodiscard]] bool check_actor_locked(const ActorContext& actor, ReasonCode& reason) const;
  [[nodiscard]] RateEnvelope* find_envelope_locked(RateEnvelopeId id);
  [[nodiscard]] const RateEnvelope* find_envelope_locked(RateEnvelopeId id) const;
  [[nodiscard]] EnforcementAttempt* find_attempt_locked(EnforcementAttemptId id);
  void write_incarnation_banner_locked();
  [[nodiscard]] bool journaling_locked() const;
  [[nodiscard]] Status durable_status_locked(JournalStatus status, const std::string& message);
  [[nodiscard]] bool retain_attempt_locked(const EnforcementAttempt& attempt);
  void build_snapshot_records_locked(std::vector<JournalRecord>& out) const;
  void collect_session_lost_updates_locked(std::vector<RateEnvelope>& out) const;
  [[nodiscard]] Status commit_many_locked(const StagedRecords& records);
  [[nodiscard]] EnvelopeAction derive_action_locked(const RateEnvelope& envelope, TimestampNs now,
                                                    ReasonCode& action_reason) const;

  EngineConfig config_{};
  IClock* clock_{nullptr};
  IEnforcementBackend* backend_{nullptr};
  Journal* journal_{nullptr};
  IEventSink* sink_{nullptr};

  mutable std::mutex mutex_;
  FabricEpoch epoch_{};
  bool backend_bound_{false};
  FabricEpoch backend_session_epoch_{};
  bool durability_degraded_{false};
  std::string durability_message_;

  std::map<u64, Flow> flows_;
  std::map<u64, Grant> grants_;
  std::map<u64, Reservation> reservations_;
  std::map<u64, Policy> policies_;
  std::map<u64, Resource> resources_;
  std::map<u64, BackendDescriptor> backends_;

  std::map<u64, RateEnvelope> envelopes_;
  std::map<u64, EnforcementAttempt> attempts_;
  std::vector<u64> attempt_order_;
  std::map<u64, std::vector<u64>> attempts_by_envelope_;

  std::deque<AuditEvent> events_;
  u64 next_envelope_id_{1};
  u64 next_attempt_id_{1};
  u64 next_event_sequence_{1};
  u64 next_txn_id_{1};
  u64 attempt_sequence_{0};
  u64 events_dropped_{0};
  u64 recovery_orphan_records_{0};
  TimestampNs last_tick_ns_{0};
  bool tick_seen_{false};

  EngineCounters counters_{};
  std::atomic<bool> shutting_down_{false};
};

[[nodiscard]] std::string describe(const EnvelopeView& view);
[[nodiscard]] std::string_view to_string_view(DurabilityMode mode) noexcept;

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_ENGINE_HPP
