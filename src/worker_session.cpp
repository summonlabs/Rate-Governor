#include "rate_governor/worker_session.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "rate_governor/version.hpp"

namespace rate_governor {

WorkerSession::WorkerSession(WorkerSessionConfig config, Socket socket)
    : config_(std::move(config)),
      socket_(std::move(socket)),
      // A worker admits nobody until a coordinator completes the handshake.
      device_(std::make_shared<SyntheticDevice>(config_.device, config_.boot,
                                                config_.starting_epoch, WorkerBootId{})) {
  accounting_.queue_capacity = static_cast<u64>(config_.queue_capacity);
}

WorkerSession::WorkerSession(WorkerSessionConfig config, Socket socket,
                             std::shared_ptr<SyntheticDevice> device)
    : config_(std::move(config)), socket_(std::move(socket)), device_(std::move(device)) {
  accounting_.queue_capacity = static_cast<u64>(config_.queue_capacity);
}

WorkerSession::~WorkerSession() {
  request_stop("session destroyed");
  stop_workers();
}

void WorkerSession::request_stop(std::string reason) {
  const bool already = stopping_.exchange(true, std::memory_order_acq_rel);
  if (!already) {
    std::lock_guard<std::mutex> guard(state_mutex_);
    if (last_error_.empty()) {
      last_error_ = std::move(reason);
    }
  }
  // Closing (not merely shutting down) is what reliably cancels a blocking
  // receive on every supported platform. Socket::close() is idempotent and
  // thread safe, so the reader's own close in run() is a harmless no-op.
  socket_.close();
}

bool WorkerSession::send(Frame frame) {
  std::lock_guard<std::mutex> guard(write_mutex_);
  std::string error;
  if (!socket_.send_frame(frame, error)) {
    request_stop("response send failed: " + error);
    return false;
  }
  return true;
}

bool WorkerSession::enqueue(PendingFrame pending) {
  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    if (queue_closed_) {
      return false;
    }
    if (queue_.size() >= config_.queue_capacity) {
      return false;
    }
    queue_.push_back(std::move(pending));
    const u64 depth = static_cast<u64>(queue_.size());
    std::lock_guard<std::mutex> state_guard(state_mutex_);
    if (depth > accounting_.queue_high_water) {
      accounting_.queue_high_water = depth;
    }
  }
  queue_cv_.notify_one();
  return true;
}

void WorkerSession::reader_loop() {
  const bool verbose = config_.verbose;
  while (!stopping_.load(std::memory_order_acquire)) {
    Frame frame;
    std::string error;
    ReasonCode reason = ReasonCode::None;
    if (!socket_.recv_frame(frame, error, reason)) {
      {
        std::lock_guard<std::mutex> guard(state_mutex_);
        if (reason == ReasonCode::MalformedInput || reason == ReasonCode::UnsupportedOperation ||
            reason == ReasonCode::CapacityLimitExceeded) {
          ++accounting_.malformed_frames;
          last_error_ = "malformed frame: " + error;
        } else if (last_error_.empty()) {
          last_error_ = error;
        }
      }
      if (verbose) {
        std::fprintf(stderr, "[rate-governor-worker] receive ended: %s\n", error.c_str());
      }
      break;
    }

    if (frame.type == MessageType::ShutdownRequest) {
      Frame response;
      response.type = MessageType::ShutdownResponse;
      AccountingResponseMessage accounting = wire_accounting();
      response.payload = encode_message(accounting);
      (void)send(std::move(response));
      {
        std::lock_guard<std::mutex> guard(state_mutex_);
        ++accounting_.shutdown_requests;
      }
      shutdown_requested_.store(true, std::memory_order_release);
      stopping_.store(true, std::memory_order_release);
      break;
    }

    const u64 sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    PendingFrame pending;
    pending.frame = std::move(frame);
    pending.sequence = sequence;
    if (!enqueue(std::move(pending))) {
      Frame rejection;
      rejection.type = MessageType::ErrorResponse;
      ErrorMessage message;
      message.reason = ReasonCode::BackendQueueFull;
      message.detail = "worker queue is full or closed";
      rejection.payload = encode_message(message);
      {
        std::lock_guard<std::mutex> guard(state_mutex_);
        ++accounting_.queue_rejections;
        ++accounting_.rejections;
      }
      (void)send(std::move(rejection));
    }
  }
}

void WorkerSession::handle_frame(const Frame& frame) {
  switch (frame.type) {
    case MessageType::Hello: {
      HelloMessage hello;
      if (!decode_message(frame.payload, hello)) {
        Frame rejection;
        rejection.type = MessageType::ErrorResponse;
        ErrorMessage message;
        message.reason = ReasonCode::BackendRejectedMalformedRequest;
        message.detail = "hello could not be decoded";
        rejection.payload = encode_message(message);
        (void)send(std::move(rejection));
        {
          std::lock_guard<std::mutex> guard(state_mutex_);
          ++accounting_.malformed_frames;
        }
        stopping_.store(true, std::memory_order_release);
        return;
      }
      ReasonCode reason = ReasonCode::None;
      if (!device_->admit_incarnation(hello.epoch, hello.boot, reason)) {
        Frame rejection;
        rejection.type = MessageType::ErrorResponse;
        ErrorMessage message;
        message.reason = reason;
        message.detail = reason == ReasonCode::BackendFencedStaleEpoch
                             ? "coordinator epoch is stale relative to the admitted epoch"
                             : "coordinator incarnation conflicts with the admitted boot id";
        rejection.payload = encode_message(message);
        {
          std::lock_guard<std::mutex> guard(state_mutex_);
          if (reason == ReasonCode::BackendFencedStaleEpoch) {
            ++accounting_.fenced_stale_epoch;
          } else {
            ++accounting_.fenced_stale_boot;
          }
          ++accounting_.rejections;
        }
        (void)send(std::move(rejection));
        stopping_.store(true, std::memory_order_release);
        return;
      }
      HelloMessage reply;
      reply.backend = config_.backend;
      reply.backend_generation = config_.backend_generation;
      reply.boot = config_.boot;
      reply.epoch = device_->highest_epoch();
      reply.kind = BackendKind::Synthetic;
      reply.capabilities =
          kBackendCapApply | kBackendCapRevoke | kBackendCapVerify | kBackendCapBurst;
      reply.verification = VerificationMode::PostApplyReadback;
      reply.max_rate_ups = config_.device.max_rate_ups;
      reply.max_burst_tokens = config_.device.max_burst_tokens;
      reply.process_id = 0;
      reply.started_at_ns = 0;
      reply.envelope_limit = config_.device.envelope_limit;
      reply.queue_capacity = static_cast<u64>(config_.queue_capacity);
      reply.worker_threads = config_.threads;
      reply.label = config_.label;
      reply.build = config_.build;
      Frame response;
      response.type = MessageType::HelloAck;
      response.payload = encode_message(reply);
      (void)send(std::move(response));
      return;
    }

    case MessageType::ApplyRequest: {
      ApplyRequestMessage request;
      if (!decode_message(frame.payload, request)) {
        Frame rejection;
        rejection.type = MessageType::ErrorResponse;
        ErrorMessage message;
        message.reason = ReasonCode::BackendRejectedMalformedRequest;
        message.detail = "apply request could not be decoded";
        rejection.payload = encode_message(message);
        (void)send(std::move(rejection));
        std::lock_guard<std::mutex> guard(state_mutex_);
        ++accounting_.malformed_frames;
        return;
      }
      ApplyRequest internal;
      internal.attempt = request.attempt;
      internal.envelope = request.envelope;
      internal.envelope_generation = request.envelope_generation;
      internal.rate_ups = request.rate_ups;
      internal.burst_tokens = request.burst_tokens;
      internal.refill = request.refill;
      internal.epoch = request.epoch;
      internal.boot = request.boot;
      const DispatchOutcome outcome = device_->apply(internal);
      ApplyResponseMessage response;
      response.attempt = request.attempt;
      response.accepted = outcome.status == DispatchStatus::Completed;
      response.reason = outcome.reason;
      response.applied_rate_ups = outcome.applied_rate_ups;
      response.applied_burst_tokens = outcome.applied_burst_tokens;
      response.epoch = outcome.epoch;
      response.boot = outcome.boot;
      response.detail = outcome.detail.empty() ? std::string("ok") : outcome.detail;
      Frame out;
      out.type = MessageType::ApplyResponse;
      out.payload = encode_message(response);
      (void)send(std::move(out));
      std::lock_guard<std::mutex> guard(state_mutex_);
      if (response.accepted) {
        ++accounting_.applies_applied;
      } else {
        ++accounting_.rejections;
      }
      return;
    }

    case MessageType::RevokeRequest: {
      RevokeRequestMessage request;
      if (!decode_message(frame.payload, request)) {
        Frame rejection;
        rejection.type = MessageType::ErrorResponse;
        ErrorMessage message;
        message.reason = ReasonCode::BackendRejectedMalformedRequest;
        message.detail = "revoke request could not be decoded";
        rejection.payload = encode_message(message);
        (void)send(std::move(rejection));
        std::lock_guard<std::mutex> guard(state_mutex_);
        ++accounting_.malformed_frames;
        return;
      }
      RevokeRequest internal;
      internal.attempt = request.attempt;
      internal.envelope = request.envelope;
      internal.envelope_generation = request.envelope_generation;
      internal.epoch = request.epoch;
      internal.boot = request.boot;
      internal.reason = request.reason;
      const DispatchOutcome outcome = device_->revoke(internal);
      RevokeResponseMessage response;
      response.attempt = request.attempt;
      response.removed = outcome.status == DispatchStatus::Completed;
      response.reason = outcome.reason;
      response.epoch = outcome.epoch;
      response.boot = outcome.boot;
      response.detail = outcome.detail.empty() ? std::string("ok") : outcome.detail;
      Frame out;
      out.type = MessageType::RevokeResponse;
      out.payload = encode_message(response);
      (void)send(std::move(out));
      std::lock_guard<std::mutex> guard(state_mutex_);
      if (response.removed) {
        ++accounting_.revokes_applied;
      } else {
        ++accounting_.rejections;
      }
      return;
    }

    case MessageType::VerifyRequest: {
      VerifyRequestMessage request;
      if (!decode_message(frame.payload, request)) {
        Frame rejection;
        rejection.type = MessageType::ErrorResponse;
        ErrorMessage message;
        message.reason = ReasonCode::BackendRejectedMalformedRequest;
        message.detail = "verify request could not be decoded";
        rejection.payload = encode_message(message);
        (void)send(std::move(rejection));
        std::lock_guard<std::mutex> guard(state_mutex_);
        ++accounting_.malformed_frames;
        return;
      }
      ApplyRequest internal;
      internal.attempt = request.attempt;
      internal.envelope = request.envelope;
      internal.envelope_generation = request.envelope_generation;
      internal.epoch = request.epoch;
      internal.boot = request.boot;
      const VerificationOutcome outcome = device_->read_back(internal);
      VerifyResponseMessage response;
      response.attempt = request.attempt;
      response.present = outcome.observed_present;
      response.observed_rate_ups = outcome.observed_rate_ups;
      response.observed_burst_tokens = outcome.observed_burst_tokens;
      response.epoch = outcome.epoch;
      response.boot = outcome.boot;
      response.reason = outcome.reason;
      response.detail = outcome.detail.empty() ? std::string("ok") : outcome.detail;
      Frame out;
      out.type = MessageType::VerifyResponse;
      out.payload = encode_message(response);
      (void)send(std::move(out));
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++accounting_.verifications;
      return;
    }

    case MessageType::HeartbeatRequest: {
      Frame out;
      out.type = MessageType::HeartbeatResponse;
      AccountingResponseMessage beat;
      beat.boot = config_.boot;
      beat.epoch = device_->highest_epoch();
      out.payload = encode_message(beat);
      (void)send(std::move(out));
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++accounting_.idle_noop_requests;
      return;
    }

    case MessageType::AccountingRequest: {
      Frame out;
      out.type = MessageType::AccountingResponse;
      AccountingResponseMessage accounting = wire_accounting();
      out.payload = encode_message(accounting);
      (void)send(std::move(out));
      return;
    }

    default: {
      Frame rejection;
      rejection.type = MessageType::ErrorResponse;
      ErrorMessage message;
      message.reason = ReasonCode::UnsupportedOperation;
      message.detail = "worker does not serve this message type";
      rejection.payload = encode_message(message);
      (void)send(std::move(rejection));
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++accounting_.rejections;
      return;
    }
  }
}

void WorkerSession::worker_loop() {
  for (;;) {
    PendingFrame pending;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() { return queue_closed_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (queue_closed_) {
          return;
        }
        continue;
      }
      pending = std::move(queue_.front());
      queue_.pop_front();
    }
    if (stopping_.load(std::memory_order_acquire) && pending.frame.type != MessageType::Hello) {
      // Work that arrives after the stop signal is never started, so it can
      // never report success.
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++accounting_.rejections;
      continue;
    }
    handle_frame(pending.frame);
    {
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++accounting_.requests_served;
    }
  }
}

void WorkerSession::stop_workers() {
  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    queue_closed_ = true;
  }
  queue_cv_.notify_all();
  for (std::thread& thread : workers_) {
    if (thread.joinable()) {
      // Joined without holding the queue or state mutex, so a worker that needs
      // either lock can still finish.
      thread.join();
    }
  }
  workers_.clear();
}

void WorkerSession::run() {
  workers_.reserve(config_.threads);
  for (std::uint32_t index = 0; index < config_.threads; ++index) {
    workers_.emplace_back([this]() { worker_loop(); });
  }
  reader_loop();
  stopping_.store(true, std::memory_order_release);
  stop_workers();
  socket_.close();
  stopped_.store(true, std::memory_order_release);
}

WorkerSessionAccounting WorkerSession::accounting() const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  return accounting_;
}

AccountingResponseMessage WorkerSession::wire_accounting_locked() const {
  AccountingResponseMessage message;
  const DeviceAccounting device = device_->accounting();
  message.requests_served = accounting_.requests_served;
  message.applies_applied = accounting_.applies_applied;
  message.revokes_applied = accounting_.revokes_applied;
  message.verifications = accounting_.verifications;
  message.rejections = accounting_.rejections;
  message.malformed_frames = accounting_.malformed_frames;
  message.fenced_stale_epoch = accounting_.fenced_stale_epoch;
  message.fenced_stale_boot = accounting_.fenced_stale_boot;
  message.queue_rejections = accounting_.queue_rejections;
  message.queue_capacity = accounting_.queue_capacity;
  message.queue_high_water = accounting_.queue_high_water;
  message.active_envelopes = static_cast<u64>(device_->entry_count());
  message.envelope_limit = config_.device.envelope_limit;
  message.idle_noop_requests = accounting_.idle_noop_requests;
  message.boot = config_.boot;
  message.epoch = device_->highest_epoch();
  return message;
}

AccountingResponseMessage WorkerSession::wire_accounting() const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  return wire_accounting_locked();
}

FabricEpoch WorkerSession::admitted_epoch() const { return device_->highest_epoch(); }

std::string WorkerSession::last_error() const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  return last_error_;
}

}  // namespace rate_governor
