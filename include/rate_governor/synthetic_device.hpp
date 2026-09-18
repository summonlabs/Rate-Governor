#ifndef RATE_GOVERNOR_SYNTHETIC_DEVICE_HPP
#define RATE_GOVERNOR_SYNTHETIC_DEVICE_HPP

// SYNTHETIC enforcement device.
//
// This is a software table with a readback. It is not a NIC, a DPU, a shaper,
// a policer, a token bucket in a kernel, or a queue discipline. Nothing in this
// repository claims that a rate configured here has any effect on a packet.
// Its purpose is to make enforcement *semantics* - apply, read back, fence,
// revoke - executable and testable without pretending to be hardware.
//
// The synthetic backend is also the only backend in this release. There is no
// physical backend implementation and no physical validation claim.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rate_governor/backend.hpp"

namespace rate_governor {

struct SyntheticDeviceConfig {
  // Declared hard limits. Zero means "not declared", which is recorded as an
  // unknown capability rather than as infinite authority.
  u64 max_rate_ups{0};
  u64 max_burst_tokens{0};
  u64 envelope_limit{4096};

  // Deterministic failure injection. Counters, not randomness: the nth call
  // behaves differently, so a failing scenario is exactly reproducible.
  u64 fail_every_nth_apply{0};
  u64 refuse_every_nth_verify{0};
  u64 underreport_every_nth_verify{0};
  u64 overreport_every_nth_verify{0};
  bool refuse_all_applies{false};
  bool refuse_all_verifies{false};
  // Spin work per apply, used to widen the dispatch window in race tests.
  u64 apply_spins{0};
};

struct SyntheticEntry {
  RateEnvelopeId envelope{};
  Generation generation{};
  u64 rate_ups{0};
  u64 burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  u64 applies{0};
  u64 revokes{0};
};

struct DeviceAccounting {
  u64 applies{0};
  u64 revokes{0};
  u64 verifications{0};
  u64 refusals{0};
  u64 under_deliveries{0};
  u64 over_deliveries{0};
  u64 injection_triggers{0};
  u64 entries_peak{0};
  u64 epoch_advances{0};
  u64 table_clears{0};
  u64 fenced_stale_epoch{0};
  u64 fenced_stale_boot{0};
  u64 limit_rejections{0};
};

class SyntheticDevice {
 public:
  // "admitted_boot" is the coordinator incarnation allowed to mutate the
  // device. A device that will be driven by a remote coordinator starts with
  // nobody admitted; an in-process device is owned by the process that also
  // coordinates it, so the two coincide.
  explicit SyntheticDevice(SyntheticDeviceConfig config, WorkerBootId boot, FabricEpoch epoch,
                           WorkerBootId admitted_boot = WorkerBootId{});

  [[nodiscard]] DispatchOutcome apply(const ApplyRequest& request);
  [[nodiscard]] DispatchOutcome revoke(const RevokeRequest& request);
  [[nodiscard]] VerificationOutcome read_back(const ApplyRequest& request);

  [[nodiscard]] DeviceAccounting accounting() const;
  [[nodiscard]] usize entry_count() const;
  [[nodiscard]] bool lookup(RateEnvelopeId envelope, SyntheticEntry& out) const;
  [[nodiscard]] FabricEpoch highest_epoch() const;
  // The incarnation identity of the process that owns this device. It is what
  // answers a readback, and what a response is attributed to.
  [[nodiscard]] WorkerBootId session_boot() const;
  // The coordinator incarnation currently admitted to mutate the device.
  [[nodiscard]] WorkerBootId admitted_boot() const;
  [[nodiscard]] const SyntheticDeviceConfig& config() const noexcept { return config_; }

  // Re-binding the session incarnation. A strictly newer epoch fences all
  // previously held enforcement state: the table is cleared, because an epoch
  // advance means every prior enforcement decision is no longer current.
  [[nodiscard]] bool admit_incarnation(FabricEpoch epoch, WorkerBootId boot, ReasonCode& reason);
  void force_clear();

 private:
  mutable std::mutex mutex_;
  SyntheticDeviceConfig config_{};
  WorkerBootId boot_{};
  FabricEpoch highest_epoch_{};
  WorkerBootId admitted_boot_{};
  std::map<u64, SyntheticEntry> table_;
  DeviceAccounting accounting_{};
  u64 apply_calls_{0};
  u64 verify_calls_{0};
};

// In-process backend adapter over a SyntheticDevice.
class SyntheticBackend final : public IEnforcementBackend {
 public:
  SyntheticBackend(BackendId id, Generation generation, SyntheticDeviceConfig config,
                   WorkerBootId boot, FabricEpoch epoch, std::string label);

  [[nodiscard]] BackendDescriptor describe() const override;
  [[nodiscard]] DispatchOutcome dispatch_apply(const ApplyRequest& request) override;
  [[nodiscard]] DispatchOutcome dispatch_revoke(const RevokeRequest& request) override;
  [[nodiscard]] VerificationOutcome verify(const ApplyRequest& request) override;
  void shutdown() noexcept override {}

  [[nodiscard]] WorkerBootId session_boot() const override { return device_.session_boot(); }
  [[nodiscard]] FabricEpoch session_epoch() const override { return device_.highest_epoch(); }
  [[nodiscard]] bool session_live() const override { return live_; }
  [[nodiscard]] std::string session_description() const override;
  [[nodiscard]] DeviceAccounting device_accounting() const { return device_.accounting(); }
  [[nodiscard]] SyntheticDevice& device() noexcept { return device_; }
  [[nodiscard]] const SyntheticDevice& device() const noexcept { return device_; }
  void set_live(bool live) noexcept { live_ = live; }

 private:
  BackendDescriptor descriptor_{};
  SyntheticDevice device_;
  bool live_{true};
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_SYNTHETIC_DEVICE_HPP
