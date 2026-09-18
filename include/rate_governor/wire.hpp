#ifndef RATE_GOVERNOR_WIRE_HPP
#define RATE_GOVERNOR_WIRE_HPP

// Framed backend transport.
//
// Every frame is magic-tagged, version-tagged, length-prefixed inside a hard
// bound and CRC-32C protected. A decoder never trusts a length before bounding
// it, never allocates from an attacker-supplied size, and never accepts a
// frame whose integrity check fails. Malformed input is refused, not repaired.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rate_governor/backend.hpp"
#include "rate_governor/checked_math.hpp"
#include "rate_governor/codec.hpp"

namespace rate_governor {

inline constexpr u32 kWireFrameMagic = 0x31464752U;  // "RGF1"
inline constexpr usize kWireHeaderSize = 16;
inline constexpr usize kWireCrcSize = 4;
inline constexpr usize kWireTrailerSize = kWireHeaderSize + kWireCrcSize;
inline constexpr u32 kWireMaxPayloadBytes = 64U * 1024U;
inline constexpr usize kWireMaxFrameBytes = kWireTrailerSize + kWireMaxPayloadBytes;
inline constexpr usize kWireMaxTextBytes = 96;

enum class MessageType : std::uint16_t {
  Unknown = 0,
  Hello = 1,
  HelloAck = 2,
  ApplyRequest = 10,
  ApplyResponse = 11,
  RevokeRequest = 12,
  RevokeResponse = 13,
  VerifyRequest = 14,
  VerifyResponse = 15,
  HeartbeatRequest = 16,
  HeartbeatResponse = 17,
  ShutdownRequest = 18,
  ShutdownResponse = 19,
  ErrorResponse = 20,
  AccountingRequest = 21,
  AccountingResponse = 22,
};

[[nodiscard]] std::string_view to_string_view(MessageType type) noexcept;

struct Frame {
  MessageType type{MessageType::Unknown};
  u32 flags{0};
  std::vector<std::byte> payload;
};

struct FrameHeader {
  u32 magic{0};
  u16 version{0};
  MessageType type{MessageType::Unknown};
  u32 flags{0};
  u32 payload_len{0};
};

// Serialises header + payload + CRC-32C(header-without-magic-device || payload).
[[nodiscard]] std::vector<std::byte> encode_frame(const Frame& frame);

// Validates header fields only. Never reads beyond header_size bytes.
[[nodiscard]] bool decode_frame_header(std::span<const std::byte> header, FrameHeader& out,
                                       ReasonCode& reason) noexcept;

// Validates a whole frame body (header + payload + crc) and copies the payload.
[[nodiscard]] bool decode_frame(std::span<const std::byte> bytes, Frame& out, ReasonCode& reason);

// ---------------------------------------------------------------------------
// Message bodies
// ---------------------------------------------------------------------------
struct HelloMessage {
  BackendId backend{};
  Generation backend_generation{Generation(1)};
  WorkerBootId boot{};
  FabricEpoch epoch{};
  BackendKind kind{BackendKind::Unknown};
  std::uint32_t capabilities{0};
  VerificationMode verification{VerificationMode::Unknown};
  u64 max_rate_ups{0};
  u64 max_burst_tokens{0};
  u64 process_id{0};
  u64 started_at_ns{0};
  u64 envelope_limit{0};
  u64 queue_capacity{0};
  std::uint32_t worker_threads{0};
  std::string label;
  std::string build;
};

struct ApplyRequestMessage {
  EnforcementAttemptId attempt{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  u64 rate_ups{0};
  u64 burst_tokens{0};
  RefillSemantics refill{RefillSemantics::Unknown};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  u64 deadline_ns{0};
};

struct ApplyResponseMessage {
  EnforcementAttemptId attempt{};
  bool accepted{false};
  ReasonCode reason{ReasonCode::None};
  u64 applied_rate_ups{0};
  u64 applied_burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  std::string detail;
};

struct RevokeRequestMessage {
  EnforcementAttemptId attempt{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  ReasonCode reason{ReasonCode::None};
};

struct RevokeResponseMessage {
  EnforcementAttemptId attempt{};
  bool removed{false};
  ReasonCode reason{ReasonCode::None};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  std::string detail;
};

struct VerifyRequestMessage {
  EnforcementAttemptId attempt{};
  RateEnvelopeId envelope{};
  Generation envelope_generation{};
  FabricEpoch epoch{};
  WorkerBootId boot{};
};

struct VerifyResponseMessage {
  EnforcementAttemptId attempt{};
  bool present{false};
  u64 observed_rate_ups{0};
  u64 observed_burst_tokens{0};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

struct AccountingResponseMessage {
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
  u64 active_envelopes{0};
  u64 envelope_limit{0};
  u64 idle_noop_requests{0};
  WorkerBootId boot{};
  FabricEpoch epoch{};
};

struct ErrorMessage {
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

[[nodiscard]] std::vector<std::byte> encode_message(const HelloMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, HelloMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const ApplyRequestMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, ApplyRequestMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const ApplyResponseMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, ApplyResponseMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const RevokeRequestMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, RevokeRequestMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const RevokeResponseMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, RevokeResponseMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const VerifyRequestMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, VerifyRequestMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const VerifyResponseMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, VerifyResponseMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const AccountingResponseMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, AccountingResponseMessage& out);
[[nodiscard]] std::vector<std::byte> encode_message(const ErrorMessage& message);
[[nodiscard]] bool decode_message(std::span<const std::byte> payload, ErrorMessage& out);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_WIRE_HPP
