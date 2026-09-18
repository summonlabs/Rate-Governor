#ifndef RATE_GOVERNOR_REMOTE_BACKEND_HPP
#define RATE_GOVERNOR_REMOTE_BACKEND_HPP

// Coordinator-side backend that reaches a real worker process over a real
// loopback socket. Every claim made through this class crossed a process
// boundary; byte and frame counters are exposed so a test can prove it.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "rate_governor/backend.hpp"
#include "rate_governor/ipc.hpp"
#include "rate_governor/wire.hpp"

namespace rate_governor {

class RemoteBackend final : public IEnforcementBackend {
 public:
  RemoteBackend() = default;
  ~RemoteBackend() override;

  // Connects, then performs the Hello/HelloAck handshake. A coordinator whose
  // epoch is stale, or whose boot id conflicts with the worker's current
  // binding at the same epoch, is refused by the worker and this returns null.
  [[nodiscard]] static std::unique_ptr<RemoteBackend> connect(const Endpoint& endpoint,
                                                              FabricEpoch epoch, WorkerBootId boot,
                                                              std::string& error);

  [[nodiscard]] BackendDescriptor describe() const override;
  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override;
  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override;
  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override;
  void shutdown() noexcept override;

  [[nodiscard]] WorkerBootId session_boot() const override { return worker_boot_; }
  [[nodiscard]] FabricEpoch session_epoch() const override { return admitted_epoch_; }
  [[nodiscard]] bool session_live() const override { return live_; }
  [[nodiscard]] std::string session_description() const override;

  [[nodiscard]] const HelloMessage& hello() const noexcept { return hello_; }
  [[nodiscard]] bool fetch_accounting(AccountingResponseMessage& out, std::string& error);
  [[nodiscard]] u64 round_trips() const;
  [[nodiscard]] u64 bytes_sent() const;
  [[nodiscard]] u64 bytes_received() const;
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
  [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }
  void mark_dead(std::string reason);

 private:
  [[nodiscard]] bool transact(const Frame& request, MessageType expected, Frame& response,
                              std::string& error);
  void note_failure(std::string error, ReasonCode reason);

  mutable std::mutex mutex_;  // one outstanding request per connection
  Socket socket_{};
  Endpoint endpoint_{};
  HelloMessage hello_{};
  BackendDescriptor descriptor_{};
  WorkerBootId worker_boot_{};
  WorkerBootId coordinator_boot_{};
  FabricEpoch admitted_epoch_{};
  u64 round_trips_{0};
  bool live_{false};
  std::string last_error_;
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_REMOTE_BACKEND_HPP
