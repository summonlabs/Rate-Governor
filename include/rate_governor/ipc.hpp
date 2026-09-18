#ifndef RATE_GOVERNOR_IPC_HPP
#define RATE_GOVERNOR_IPC_HPP

// Real OS sockets carrying real framed transport.
//
// The transport is intentionally minimal: TCP over the loopback interface only,
// blocking I/O, one frame at a time per direction, with every byte counted so a
// test can assert that a claim crossed a process boundary rather than an
// in-process shortcut. No reliability, ordering or delivery property beyond
// what TCP itself provides is claimed.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "rate_governor/checked_math.hpp"
#include "rate_governor/wire.hpp"

namespace rate_governor {

struct Endpoint {
  std::string host{"127.0.0.1"};
  u16 port{0};

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static bool parse(std::string_view text, Endpoint& out) noexcept;
  [[nodiscard]] bool loopback() const noexcept { return host == "127.0.0.1" || host == "localhost"; }
};

// Process-wide socket runtime. Safe to call repeatedly and from many threads.
void ensure_socket_runtime();

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static bool connect_to(const Endpoint& endpoint, Socket& out, std::string& error);

  // Sends exactly one framed message. Partial writes are completed or the
  // call fails; a caller never observes a half-sent frame reported as success.
  [[nodiscard]] bool send_frame(const Frame& frame, std::string& error);

  // Receives exactly one framed message. A clean peer shutdown, a short read,
  // an oversized length, a bad magic/version or a failed CRC all report false.
  [[nodiscard]] bool recv_frame(Frame& frame, std::string& error, ReasonCode& reason);

  // Cancels any blocking receive on this socket and releases the handle.
  // Idempotent and safe to call from another thread: the handle is exchanged
  // out atomically exactly once, so a shutdown path can wake its reader and
  // then join it without risking a double close.
  void close() noexcept;
  // Requests that a concurrent blocking receive return without releasing the
  // handle. Prefer close() when the socket is not needed afterwards.
  void interrupt() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] u64 bytes_sent() const noexcept { return bytes_sent_; }
  [[nodiscard]] u64 bytes_received() const noexcept { return bytes_received_; }
  [[nodiscard]] u64 frames_sent() const noexcept { return frames_sent_; }
  [[nodiscard]] u64 frames_received() const noexcept { return frames_received_; }

  // Test hooks for adversarial framing. Deliberately public: producing a
  // malformed frame is a first-class capability of the validation suite.
  [[nodiscard]] bool send_raw(std::string_view bytes, std::string& error);
  [[nodiscard]] bool recv_exact(std::string& out, usize count, std::string& error);
  [[nodiscard]] std::uintptr_t native_handle() const noexcept {
    return handle_.load(std::memory_order_acquire);
  }

 private:
  static constexpr std::uintptr_t kInvalidSocketHandle =
      static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

  explicit Socket(std::uintptr_t handle) noexcept : handle_(handle) {}
  friend class Listener;

  std::atomic<std::uintptr_t> handle_{kInvalidSocketHandle};
  u64 bytes_sent_{0};
  u64 bytes_received_{0};
  u64 frames_sent_{0};
  u64 frames_received_{0};
};

class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Binds 127.0.0.1:port (port 0 selects an ephemeral port).
  [[nodiscard]] static std::unique_ptr<Listener> bind_loopback(u16 port, std::string& error);

  [[nodiscard]] bool accept(Socket& out, std::string& error);
  [[nodiscard]] u16 port() const noexcept { return port_; }
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
  u16 port_{0};
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_IPC_HPP
