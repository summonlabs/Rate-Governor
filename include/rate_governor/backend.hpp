#ifndef RATE_GOVERNOR_BACKEND_HPP
#define RATE_GOVERNOR_BACKEND_HPP

// The narrow enforcement abstraction.
//
// This interface deliberately has no vocabulary for arbitration, admission,
// path selection, packet scheduling, priority, QoS classes or congestion
// control. It can do exactly three things to an already-authorized envelope:
// apply it, revoke it, and read back what it currently holds. Rate Governor
// owns the envelope authority; the backend owns only the physical (or
// synthetic) act.

#include <cstdint>
#include <string>
#include <string_view>

#include "rate_governor/entities.hpp"
#include "rate_governor/envelope.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

struct ApplyRequest {
  EnforcementAttemptId attempt{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  PlanFingerprint fingerprint{};
  u64 rate_ups{0};
  u64 burst_tokens{0};
  RefillSemantics refill{RefillSemantics::Unknown};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  TimestampNs issued_ns{0};
};

struct RevokeRequest {
  EnforcementAttemptId attempt{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  PlanFingerprint fingerprint{};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  ReasonCode reason{ReasonCode::None};
  TimestampNs issued_ns{0};
};

enum class DispatchStatus : std::uint8_t {
  Unknown = 0,
  // The backend finished the work and reports the effect it believes it holds.
  Completed,
  // The backend accepted the work and will complete it asynchronously. Nothing
  // is authoritative until a completion arrives and is verified.
  Pending,
  // The backend refused the work. Nothing was changed.
  Rejected,
  // The backend cannot express this operation at all.
  Unsupported,
  // The transport or the device failed. The outcome is unknown, not negative.
  Failed,
};

struct DispatchOutcome {
  DispatchStatus status{DispatchStatus::Unknown};
  ReasonCode reason{ReasonCode::None};
  u64 applied_rate_ups{0};
  u64 applied_burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  std::string detail;

  [[nodiscard]] bool completed() const noexcept { return status == DispatchStatus::Completed; }
};

enum class VerificationStatus : std::uint8_t {
  Unknown = 0,
  // The backend reported the envelope it currently holds.
  Confirmed,
  // The backend holds something other than what was requested.
  Mismatch,
  // The backend has no readback at all, so no effect can be confirmed.
  Unsupported,
  // The readback itself failed; the effect remains UNKNOWN.
  Failed,
};

struct VerificationOutcome {
  VerificationStatus status{VerificationStatus::Unknown};
  ReasonCode reason{ReasonCode::None};
  bool observed_present{false};
  u64 observed_rate_ups{0};
  u64 observed_burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  std::string detail;
};

class IEnforcementBackend {
 public:
  IEnforcementBackend() = default;
  virtual ~IEnforcementBackend();
  IEnforcementBackend(const IEnforcementBackend&) = delete;
  IEnforcementBackend& operator=(const IEnforcementBackend&) = delete;

  [[nodiscard]] virtual BackendDescriptor describe() const = 0;
  [[nodiscard]] virtual DispatchOutcome dispatch_apply(const ApplyRequest& request) = 0;
  [[nodiscard]] virtual DispatchOutcome dispatch_revoke(const RevokeRequest& request) = 0;
  [[nodiscard]] virtual VerificationOutcome verify(const ApplyRequest& request) = 0;

  // Session incarnation. A backend reached across a process boundary reports
  // the boot id of the worker actually serving it; an in-process backend
  // reports the engine's own boot id.
  // session_boot/session_epoch/session_live must be cheap, non-blocking
  // predicates over already-known session state. They are the only members the
  // engine may call while holding its state lock, and they must never call back
  // into the engine.
  [[nodiscard]] virtual WorkerBootId session_boot() const { return WorkerBootId{}; }
  [[nodiscard]] virtual FabricEpoch session_epoch() const { return FabricEpoch{}; }
  [[nodiscard]] virtual bool session_live() const { return true; }
  [[nodiscard]] virtual std::string session_description() const { return std::string("in-process"); }

  virtual void shutdown() noexcept = 0;
};

// A backend that can do nothing. Every operation is refused with an explicit
// unsupported reason: nothing is ever silently accepted.
class UnsupportedBackend final : public IEnforcementBackend {
 public:
  UnsupportedBackend(BackendId id, Generation generation, std::string label);

  [[nodiscard]] BackendDescriptor describe() const override;
  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override;
  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override;
  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override;
  void shutdown() noexcept override {}

 private:
  BackendDescriptor descriptor_{};
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_BACKEND_HPP
