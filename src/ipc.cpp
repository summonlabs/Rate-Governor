#include "rate_governor/ipc.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "rate_governor/ids.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rate_governor {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;

std::string last_socket_error() {
  const int code = WSAGetLastError();
  return std::string("winsock error ") + std::to_string(code);
}

void close_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::shutdown(socket, SD_BOTH);
    ::closesocket(socket);
  }
}

void shutdown_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::shutdown(socket, SD_BOTH);
  }
}

#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
constexpr int kSocketError = -1;

std::string last_socket_error() {
  return std::string("socket error ") + std::to_string(errno);
}

void close_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::shutdown(socket, SHUT_RDWR);
    ::close(socket);
  }
}

void shutdown_native(NativeSocket socket) noexcept {
  if (socket != kInvalidSocket) {
    ::shutdown(socket, SHUT_RDWR);
  }
}

#endif

std::once_flag g_socket_runtime_once;

NativeSocket to_native(std::uintptr_t handle) noexcept {
  return static_cast<NativeSocket>(handle);
}

std::uintptr_t to_handle(NativeSocket socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}

}  // namespace

void ensure_socket_runtime() {
  std::call_once(g_socket_runtime_once, []() {
#if defined(_WIN32)
    WSADATA data{};
    (void)::WSAStartup(MAKEWORD(2, 2), &data);
#endif
  });
}

std::string Endpoint::to_string() const {
  std::string out = host;
  out.push_back(':');
  out.append(std::to_string(port));
  return out;
}

bool Endpoint::parse(std::string_view text, Endpoint& out) noexcept {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return false;
  }
  u64 port = 0;
  if (!parse_u64_strict(text.substr(colon + 1), port) || port > 65535) {
    return false;
  }
  out.host.assign(text.substr(0, colon));
  out.port = static_cast<u16>(port);
  return true;
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_.exchange(kInvalidSocketHandle)),
      bytes_sent_(other.bytes_sent_),
      bytes_received_(other.bytes_received_),
      frames_sent_(other.frames_sent_),
      frames_received_(other.frames_received_) {
  other.bytes_sent_ = 0;
  other.bytes_received_ = 0;
  other.frames_sent_ = 0;
  other.frames_received_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_.store(other.handle_.exchange(kInvalidSocketHandle));
    bytes_sent_ = other.bytes_sent_;
    bytes_received_ = other.bytes_received_;
    frames_sent_ = other.frames_sent_;
    frames_received_ = other.frames_received_;
    other.bytes_sent_ = 0;
    other.bytes_received_ = 0;
    other.frames_sent_ = 0;
    other.frames_received_ = 0;
  }
  return *this;
}

bool Socket::valid() const noexcept {
  return handle_.load(std::memory_order_acquire) != kInvalidSocketHandle;
}

void Socket::interrupt() noexcept {
  const std::uintptr_t handle = handle_.load(std::memory_order_acquire);
  if (handle != kInvalidSocketHandle) {
    shutdown_native(to_native(handle));
  }
}

void Socket::close() noexcept {
  // Exactly one caller observes the old handle and closes it. A concurrent
  // close, or a close racing a blocking receive, therefore cannot double close
  // or close a handle that another thread is still using.
  const std::uintptr_t handle = handle_.exchange(kInvalidSocketHandle, std::memory_order_acq_rel);
  if (handle != kInvalidSocketHandle) {
    close_native(to_native(handle));
  }
}

bool Socket::connect_to(const Endpoint& endpoint, Socket& out, std::string& error) {
  ensure_socket_runtime();
  if (!endpoint.loopback()) {
    error = "only loopback endpoints are accepted";
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);

#if defined(_WIN32)
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif

  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    error = last_socket_error();
    return false;
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == kSocketError) {
    error = last_socket_error();
    close_native(socket);
    return false;
  }
  out.close();
  out.handle_.store(to_handle(socket), std::memory_order_release);
  return true;
}

bool Socket::send_raw(std::string_view bytes, std::string& error) {
  if (!valid()) {
    error = "socket is not connected";
    return false;
  }
  const char* data = bytes.data();
  usize remaining = bytes.size();
  while (remaining > 0) {
    const int chunk =
        ::send(to_native(handle_.load(std::memory_order_acquire)), data,
               static_cast<int>(remaining), 0);
    if (chunk == kSocketError || chunk == 0) {
      error = last_socket_error();
      return false;
    }
    data += chunk;
    remaining -= static_cast<usize>(chunk);
    bytes_sent_ += static_cast<u64>(chunk);
  }
  return true;
}

bool Socket::send_frame(const Frame& frame, std::string& error) {
  const std::vector<std::byte> encoded = encode_frame(frame);
  if (encoded.empty()) {
    error = "frame could not be encoded within the configured bound";
    return false;
  }
  const std::string_view view(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  if (!send_raw(view, error)) {
    return false;
  }
  ++frames_sent_;
  return true;
}

bool Socket::recv_exact(std::string& out, usize count, std::string& error) {
  out.clear();
  out.resize(count);
  if (!valid()) {
    error = "socket is not connected";
    return false;
  }
  usize received = 0;
  while (received < count) {
    const int chunk = ::recv(to_native(handle_.load(std::memory_order_acquire)),
                             out.data() + received, static_cast<int>(count - received), 0);
    if (chunk == 0) {
      error = "peer closed the connection";
      return false;
    }
    if (chunk == kSocketError) {
      error = last_socket_error();
      return false;
    }
    received += static_cast<usize>(chunk);
    bytes_received_ += static_cast<u64>(chunk);
  }
  return true;
}

bool Socket::recv_frame(Frame& frame, std::string& error, ReasonCode& reason) {
  reason = ReasonCode::None;
  std::string header_bytes;
  if (!recv_exact(header_bytes, kWireHeaderSize, error)) {
    return false;
  }
  FrameHeader header;
  const std::span<const std::byte> header_view(
      reinterpret_cast<const std::byte*>(header_bytes.data()), header_bytes.size());
  if (!decode_frame_header(header_view, header, reason)) {
    error = "frame header rejected";
    return false;
  }
  std::string body;
  if (!recv_exact(body, static_cast<usize>(header.payload_len) + kWireCrcSize, error)) {
    return false;
  }
  std::vector<std::byte> whole;
  whole.reserve(kWireHeaderSize + body.size());
  const std::span<const std::byte> body_view(reinterpret_cast<const std::byte*>(body.data()),
                                             body.size());
  whole.insert(whole.end(), header_view.begin(), header_view.end());
  whole.insert(whole.end(), body_view.begin(), body_view.end());
  if (!decode_frame(whole, frame, reason)) {
    error = "frame body rejected";
    return false;
  }
  ++frames_received_;
  return true;
}

Listener::~Listener() { close(); }

std::unique_ptr<Listener> Listener::bind_loopback(u16 port, std::string& error) {
  ensure_socket_runtime();
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    error = last_socket_error();
    return nullptr;
  }
  int reuse = 1;
  (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == kSocketError) {
    error = last_socket_error();
    close_native(socket);
    return nullptr;
  }
  if (::listen(socket, 8) == kSocketError) {
    error = last_socket_error();
    close_native(socket);
    return nullptr;
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = static_cast<int>(sizeof(bound));
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) == kSocketError) {
    error = last_socket_error();
    close_native(socket);
    return nullptr;
  }
  auto listener = std::unique_ptr<Listener>(new Listener());
  listener->handle_ = to_handle(socket);
  listener->port_ = ntohs(bound.sin_port);
  return listener;
}

bool Listener::accept(Socket& out, std::string& error) {
  if (!valid()) {
    error = "listener is not bound";
    return false;
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = static_cast<int>(sizeof(peer));
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const NativeSocket client =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (client == kInvalidSocket) {
    error = last_socket_error();
    return false;
  }
  out.close();
  out.handle_.store(to_handle(client), std::memory_order_release);
  return true;
}

void Listener::close() noexcept {
  if (valid()) {
    close_native(to_native(handle_));
    handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
  }
}

bool Listener::valid() const noexcept {
  return handle_ != static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
}

}  // namespace rate_governor
