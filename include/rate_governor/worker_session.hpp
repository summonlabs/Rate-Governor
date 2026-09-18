#ifndef RATE_GOVERNOR_WORKER_SESSION_HPP
#define RATE_GOVERNOR_WORKER_SESSION_HPP

// Worker-side session server.
//
// Real threads, a real bounded queue and a real socket. Shutdown stops
// accepting work, signals active work, drains only work that completed, and
// joins its pool without holding any lock the pool needs - so the accounting
// returns to a valid baseline and no cancelled request can report success.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rate_governor/ipc.hpp"
#include "rate_governor/synthetic_device.hpp"
#include "rate_governor/wire.hpp"

namespace rate_governor {

struct WorkerSessionConfig {
  BackendId backend{};
  Generation backend_generation{Generation(1)};
  WorkerBootId boot{};
  FabricEpoch starting_epoch{};
  std::uint32_t threads{2};
  usize queue_capacity{64};
  std::string label{"rate-governor-synthetic-worker"};
  std::string build{"rate-governor/1.0.0 synthetic worker"};
  SyntheticDeviceConfig device{};
  bool verbose{false};
};

struct WorkerSessionAccounting {
  u64 requests_served{0};
  u64 applies_applied{0};
  u64 revokes_applied{0};
  u64 verifications{0};
  u64 rejections{0};
  u64 malformed_frames{0};
  u64 fenced_stale_epoch{0};
  u64 fenced_stale_boot{0};
  u64 queue_rejections{0};
  u64 queue_capacity{0};
  u64 queue_high_water{0};
  u64 idle_noop_requests{0};
  u64 shutdown_requests{0};
  bool stopped{false};
};

class WorkerSession {
 public:
  WorkerSession(WorkerSessionConfig config, Socket socket);
  // Shares an already-created device. A worker that serves several concurrent
  // sessions from one process must share one device, otherwise each connection
  // would see a different enforcement world.
  WorkerSession(WorkerSessionConfig config, Socket socket, std::shared_ptr<SyntheticDevice> device);
  ~WorkerSession();
  WorkerSession(const WorkerSession&) = delete;
  WorkerSession& operator=(const WorkerSession&) = delete;

  // Runs the reader loop on the calling thread until a shutdown request, peer
  // EOF, or request_stop(). Always joins the pool before returning.
  void run();

  // Safe to call from another thread. Wakes the reader by shutting the socket
  // down and marks the session as stopping.
  void request_stop(std::string reason);

  [[nodiscard]] WorkerSessionAccounting accounting() const;
  [[nodiscard]] AccountingResponseMessage wire_accounting() const;
  [[nodiscard]] WorkerBootId boot() const noexcept { return config_.boot; }
  [[nodiscard]] FabricEpoch admitted_epoch() const;
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }
  // True once an explicit shutdown request was served. A listener that wants to
  // exit after serving one session uses this to stop accepting.
  [[nodiscard]] bool shutdown_requested() const noexcept {
    return shutdown_requested_.load(std::memory_order_acquire);
  }

 private:
  struct PendingFrame {
    Frame frame{};
    u64 sequence{0};
  };

  [[nodiscard]] AccountingResponseMessage wire_accounting_locked() const;
  void reader_loop();
  void worker_loop();
  void handle_frame(const Frame& frame);
  [[nodiscard]] bool enqueue(PendingFrame pending);
  [[nodiscard]] bool send(Frame frame);
  void stop_workers();

  WorkerSessionConfig config_{};
  Socket socket_{};
  std::shared_ptr<SyntheticDevice> device_;
  mutable std::mutex state_mutex_;
  WorkerSessionAccounting accounting_{};
  std::string last_error_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<PendingFrame> queue_;
  bool queue_closed_{false};

  std::mutex write_mutex_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<u64> sequence_{0};
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_WORKER_SESSION_HPP
