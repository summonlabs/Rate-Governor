#include "rate_governor/wire.hpp"

#include <cstring>

#include "rate_governor/version.hpp"

namespace rate_governor {
namespace {

void write_reason(ByteWriter& writer, ReasonCode reason) {
  writer.u16(static_cast<u16>(reason));
}

ReasonCode read_reason(ByteReader& reader) {
  u16 raw = 0;
  if (!reader.u16(raw)) {
    return ReasonCode::MalformedInput;
  }
  return static_cast<ReasonCode>(raw);
}

bool frame_checksum(std::span<const std::byte> bytes, usize header_and_payload, u32& out) {
  if (bytes.size() < header_and_payload) {
    return false;
  }
  out = crc32c(bytes.subspan(0, header_and_payload));
  return true;
}

}  // namespace

std::string_view to_string_view(MessageType type) noexcept {
  switch (type) {
    case MessageType::Unknown: return "Unknown";
    case MessageType::Hello: return "Hello";
    case MessageType::HelloAck: return "HelloAck";
    case MessageType::ApplyRequest: return "ApplyRequest";
    case MessageType::ApplyResponse: return "ApplyResponse";
    case MessageType::RevokeRequest: return "RevokeRequest";
    case MessageType::RevokeResponse: return "RevokeResponse";
    case MessageType::VerifyRequest: return "VerifyRequest";
    case MessageType::VerifyResponse: return "VerifyResponse";
    case MessageType::HeartbeatRequest: return "HeartbeatRequest";
    case MessageType::HeartbeatResponse: return "HeartbeatResponse";
    case MessageType::ShutdownRequest: return "ShutdownRequest";
    case MessageType::ShutdownResponse: return "ShutdownResponse";
    case MessageType::ErrorResponse: return "ErrorResponse";
    case MessageType::AccountingRequest: return "AccountingRequest";
    case MessageType::AccountingResponse: return "AccountingResponse";
  }
  return "Unknown";
}

std::vector<std::byte> encode_frame(const Frame& frame) {
  std::vector<std::byte> out;
  if (frame.payload.size() > kWireMaxPayloadBytes) {
    return out;
  }
  ByteWriter writer(kWireHeaderSize + frame.payload.size() + kWireCrcSize);
  writer.u32(kWireFrameMagic);
  writer.u16(kWireProtocolVersion);
  writer.u16(static_cast<u16>(frame.type));
  writer.u32(frame.flags);
  writer.u32(static_cast<u32>(frame.payload.size()));
  writer.raw(frame.payload);
  const std::span<const std::byte> header_and_payload = writer.span();
  u32 crc = 0;
  if (!frame_checksum(header_and_payload, header_and_payload.size(), crc)) {
    return out;
  }
  writer.u32(crc);
  return writer.data();
}

bool decode_frame_header(std::span<const std::byte> header, FrameHeader& out,
                         ReasonCode& reason) noexcept {
  reason = ReasonCode::None;
  if (header.size() < kWireHeaderSize) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  ByteReader reader(header.subspan(0, kWireHeaderSize));
  u16 raw_type = 0;
  if (!reader.u32(out.magic) || !reader.u16(out.version) || !reader.u16(raw_type) ||
      !reader.u32(out.flags) || !reader.u32(out.payload_len)) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  if (out.magic != kWireFrameMagic) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  if (out.version != kWireProtocolVersion) {
    reason = ReasonCode::UnsupportedOperation;
    return false;
  }
  if (out.payload_len > kWireMaxPayloadBytes) {
    reason = ReasonCode::CapacityLimitExceeded;
    return false;
  }
  out.type = static_cast<MessageType>(raw_type);
  return true;
}

bool decode_frame(std::span<const std::byte> bytes, Frame& out, ReasonCode& reason) {
  FrameHeader header;
  if (!decode_frame_header(bytes, header, reason)) {
    return false;
  }
  const usize total = kWireHeaderSize + static_cast<usize>(header.payload_len) + kWireCrcSize;
  if (bytes.size() < total) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  const usize header_and_payload = kWireHeaderSize + static_cast<usize>(header.payload_len);
  ByteReader reader(bytes.subspan(header_and_payload, kWireCrcSize));
  u32 stored_crc = 0;
  if (!reader.u32(stored_crc)) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  u32 computed = 0;
  if (!frame_checksum(bytes, header_and_payload, computed) || computed != stored_crc) {
    reason = ReasonCode::MalformedInput;
    return false;
  }
  out.type = header.type;
  out.flags = header.flags;
  out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize),
                     bytes.begin() + static_cast<std::ptrdiff_t>(header_and_payload));
  return true;
}

// ---------------------------------------------------------------------------
// Message bodies
// ---------------------------------------------------------------------------
std::vector<std::byte> encode_message(const HelloMessage& message) {
  ByteWriter writer(160);
  writer.u64(message.backend.value());
  writer.u64(message.backend_generation.value());
  writer.u64(message.boot.value());
  writer.u64(message.epoch.value());
  writer.u8(static_cast<u8>(message.kind));
  writer.u8(static_cast<u8>(message.verification));
  writer.u16(0);
  writer.u32(message.capabilities);
  writer.u64(message.max_rate_ups);
  writer.u64(message.max_burst_tokens);
  writer.u64(message.process_id);
  writer.u64(message.started_at_ns);
  writer.u64(message.envelope_limit);
  writer.u64(message.queue_capacity);
  writer.u32(message.worker_threads);
  writer.u32(static_cast<u32>(kWireProtocolVersion));
  if (!writer.text(message.label, kWireMaxTextBytes)) {
    return {};
  }
  if (!writer.text(message.build, kWireMaxTextBytes)) {
    return {};
  }
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, HelloMessage& out) {
  ByteReader reader(payload);
  u64 backend = 0;
  u64 generation = 0;
  u64 boot = 0;
  u64 epoch = 0;
  u8 kind = 0;
  u8 verification = 0;
  u16 reserved = 0;
  u32 protocol = 0;
  if (!reader.u64(backend) || !reader.u64(generation) || !reader.u64(boot) || !reader.u64(epoch) ||
      !reader.u8(kind) || !reader.u8(verification) || !reader.u16(reserved) ||
      !reader.u32(out.capabilities) || !reader.u64(out.max_rate_ups) ||
      !reader.u64(out.max_burst_tokens) || !reader.u64(out.process_id) ||
      !reader.u64(out.started_at_ns) || !reader.u64(out.envelope_limit) ||
      !reader.u64(out.queue_capacity) || !reader.u32(out.worker_threads) ||
      !reader.u32(protocol)) {
    return false;
  }
  if (reserved != 0 || protocol != kWireProtocolVersion) {
    return false;
  }
  if (!reader.text(kWireMaxTextBytes, out.label) || !reader.text(kWireMaxTextBytes, out.build)) {
    return false;
  }
  out.backend = BackendId(backend);
  out.backend_generation = Generation(generation);
  out.boot = WorkerBootId(boot);
  out.epoch = FabricEpoch(epoch);
  out.kind = static_cast<BackendKind>(kind);
  out.verification = static_cast<VerificationMode>(verification);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const ApplyRequestMessage& message) {
  ByteWriter writer(96);
  writer.u64(message.attempt.value());
  writer.u64(message.envelope.value());
  writer.u64(message.envelope_generation.value());
  writer.u64(message.rate_ups);
  writer.u64(message.burst_tokens);
  writer.u8(static_cast<u8>(message.refill));
  writer.u8(0);
  writer.u16(0);
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  writer.u64(message.deadline_ns);
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, ApplyRequestMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u64 envelope = 0;
  u64 generation = 0;
  u8 refill = 0;
  u8 pad8 = 0;
  u16 pad16 = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u64(attempt) || !reader.u64(envelope) || !reader.u64(generation) ||
      !reader.u64(out.rate_ups) || !reader.u64(out.burst_tokens) || !reader.u8(refill) ||
      !reader.u8(pad8) || !reader.u16(pad16) || !reader.u64(epoch) || !reader.u64(boot) ||
      !reader.u64(out.deadline_ns)) {
    return false;
  }
  if (pad8 != 0 || pad16 != 0) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.envelope = RateEnvelopeId(envelope);
  out.envelope_generation = Generation(generation);
  out.refill = static_cast<RefillSemantics>(refill);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const ApplyResponseMessage& message) {
  ByteWriter writer(96);
  writer.u64(message.attempt.value());
  writer.u8(message.accepted ? 1U : 0U);
  writer.u8(0);
  write_reason(writer, message.reason);
  writer.u32(0);
  writer.u64(message.applied_rate_ups);
  writer.u64(message.applied_burst_tokens);
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  if (!writer.text(message.detail, kWireMaxTextBytes)) {
    return {};
  }
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, ApplyResponseMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u8 accepted = 0;
  u8 pad8 = 0;
  u32 pad32 = 0;
  u64 epoch = 0;
  u64 boot = 0;
  ReasonCode reason = ReasonCode::None;
  if (!reader.u64(attempt) || !reader.u8(accepted) || !reader.u8(pad8)) {
    return false;
  }
  reason = read_reason(reader);
  if (!reader.ok() || !reader.u32(pad32) || !reader.u64(out.applied_rate_ups) ||
      !reader.u64(out.applied_burst_tokens) || !reader.u64(epoch) || !reader.u64(boot) ||
      !reader.text(kWireMaxTextBytes, out.detail)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0 || accepted > 1U) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.accepted = accepted == 1U;
  out.reason = reason;
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const RevokeRequestMessage& message) {
  ByteWriter writer(64);
  writer.u64(message.attempt.value());
  writer.u64(message.envelope.value());
  writer.u64(message.envelope_generation.value());
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  write_reason(writer, message.reason);
  writer.u16(0);
  writer.u32(0);
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, RevokeRequestMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u64 envelope = 0;
  u64 generation = 0;
  u64 epoch = 0;
  u64 boot = 0;
  u16 pad16 = 0;
  u32 pad32 = 0;
  if (!reader.u64(attempt) || !reader.u64(envelope) || !reader.u64(generation) ||
      !reader.u64(epoch) || !reader.u64(boot)) {
    return false;
  }
  const ReasonCode reason = read_reason(reader);
  if (!reader.ok() || !reader.u16(pad16) || !reader.u32(pad32)) {
    return false;
  }
  if (pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.envelope = RateEnvelopeId(envelope);
  out.envelope_generation = Generation(generation);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  out.reason = reason;
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const RevokeResponseMessage& message) {
  ByteWriter writer(64);
  writer.u64(message.attempt.value());
  writer.u8(message.removed ? 1U : 0U);
  writer.u8(0);
  write_reason(writer, message.reason);
  writer.u32(0);
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  if (!writer.text(message.detail, kWireMaxTextBytes)) {
    return {};
  }
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, RevokeResponseMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u8 removed = 0;
  u8 pad8 = 0;
  u32 pad32 = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u64(attempt) || !reader.u8(removed) || !reader.u8(pad8)) {
    return false;
  }
  const ReasonCode reason = read_reason(reader);
  if (!reader.ok() || !reader.u32(pad32) || !reader.u64(epoch) || !reader.u64(boot) ||
      !reader.text(kWireMaxTextBytes, out.detail)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0 || removed > 1U) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.removed = removed == 1U;
  out.reason = reason;
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const VerifyRequestMessage& message) {
  ByteWriter writer(48);
  writer.u64(message.attempt.value());
  writer.u64(message.envelope.value());
  writer.u64(message.envelope_generation.value());
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, VerifyRequestMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u64 envelope = 0;
  u64 generation = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u64(attempt) || !reader.u64(envelope) || !reader.u64(generation) ||
      !reader.u64(epoch) || !reader.u64(boot)) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.envelope = RateEnvelopeId(envelope);
  out.envelope_generation = Generation(generation);
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const VerifyResponseMessage& message) {
  ByteWriter writer(96);
  writer.u64(message.attempt.value());
  writer.u8(message.present ? 1U : 0U);
  writer.u8(0);
  write_reason(writer, message.reason);
  writer.u32(0);
  writer.u64(message.observed_rate_ups);
  writer.u64(message.observed_burst_tokens);
  writer.u64(message.epoch.value());
  writer.u64(message.boot.value());
  if (!writer.text(message.detail, kWireMaxTextBytes)) {
    return {};
  }
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, VerifyResponseMessage& out) {
  ByteReader reader(payload);
  u64 attempt = 0;
  u8 present = 0;
  u8 pad8 = 0;
  u32 pad32 = 0;
  u64 epoch = 0;
  u64 boot = 0;
  if (!reader.u64(attempt) || !reader.u8(present) || !reader.u8(pad8)) {
    return false;
  }
  const ReasonCode reason = read_reason(reader);
  if (!reader.ok() || !reader.u32(pad32) || !reader.u64(out.observed_rate_ups) ||
      !reader.u64(out.observed_burst_tokens) || !reader.u64(epoch) || !reader.u64(boot) ||
      !reader.text(kWireMaxTextBytes, out.detail)) {
    return false;
  }
  if (pad8 != 0 || pad32 != 0 || present > 1U) {
    return false;
  }
  out.attempt = EnforcementAttemptId(attempt);
  out.present = present == 1U;
  out.reason = reason;
  out.epoch = FabricEpoch(epoch);
  out.boot = WorkerBootId(boot);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const AccountingResponseMessage& message) {
  ByteWriter writer(160);
  writer.u64(message.requests_served);
  writer.u64(message.applies_applied);
  writer.u64(message.revokes_applied);
  writer.u64(message.verifications);
  writer.u64(message.rejections);
  writer.u64(message.malformed_frames);
  writer.u64(message.fenced_stale_epoch);
  writer.u64(message.fenced_stale_boot);
  writer.u64(message.queue_rejections);
  writer.u64(message.queue_capacity);
  writer.u64(message.queue_high_water);
  writer.u64(message.active_envelopes);
  writer.u64(message.envelope_limit);
  writer.u64(message.idle_noop_requests);
  writer.u64(message.boot.value());
  writer.u64(message.epoch.value());
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, AccountingResponseMessage& out) {
  ByteReader reader(payload);
  u64 boot = 0;
  u64 epoch = 0;
  if (!reader.u64(out.requests_served) || !reader.u64(out.applies_applied) ||
      !reader.u64(out.revokes_applied) || !reader.u64(out.verifications) ||
      !reader.u64(out.rejections) || !reader.u64(out.malformed_frames) ||
      !reader.u64(out.fenced_stale_epoch) || !reader.u64(out.fenced_stale_boot) ||
      !reader.u64(out.queue_rejections) || !reader.u64(out.queue_capacity) ||
      !reader.u64(out.queue_high_water) || !reader.u64(out.active_envelopes) ||
      !reader.u64(out.envelope_limit) || !reader.u64(out.idle_noop_requests) ||
      !reader.u64(boot) || !reader.u64(epoch)) {
    return false;
  }
  out.boot = WorkerBootId(boot);
  out.epoch = FabricEpoch(epoch);
  return reader.fully_consumed();
}

std::vector<std::byte> encode_message(const ErrorMessage& message) {
  ByteWriter writer(32);
  write_reason(writer, message.reason);
  writer.u16(0);
  writer.u32(0);
  if (!writer.text(message.detail, kWireMaxTextBytes)) {
    return {};
  }
  return writer.data();
}

bool decode_message(std::span<const std::byte> payload, ErrorMessage& out) {
  ByteReader reader(payload);
  u16 pad16 = 0;
  u32 pad32 = 0;
  const ReasonCode reason = read_reason(reader);
  if (!reader.ok() || !reader.u16(pad16) || !reader.u32(pad32) ||
      !reader.text(kWireMaxTextBytes, out.detail)) {
    return false;
  }
  if (pad16 != 0 || pad32 != 0) {
    return false;
  }
  out.reason = reason;
  return reader.fully_consumed();
}

}  // namespace rate_governor
