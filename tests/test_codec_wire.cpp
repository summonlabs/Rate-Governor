#include "test_support.hpp"

#include <random>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

Frame make_frame(MessageType type, std::vector<std::byte> payload) {
  Frame frame;
  frame.type = type;
  frame.payload = std::move(payload);
  return frame;
}

}  // namespace

RG_TEST(codec, crc32c_matches_the_standard_vector) {
  const std::string_view text = "123456789";
  const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                                text.size());
  RG_CHECK_EQ(crc32c(bytes), 0xE3069283U);
  RG_CHECK_EQ(crc32c({}), 0U);
  const u32 first = crc32c_extend(0, bytes.subspan(0, 4));
  const u32 whole = crc32c_extend(first, bytes.subspan(4));
  RG_CHECK_EQ(whole, crc32c(bytes));
}

RG_TEST(codec, writer_reader_round_trip) {
  ByteWriter writer(64);
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEU);
  writer.u64(0x0123456789ABCDEFULL);
  RG_CHECK(writer.text("hello", 16));
  writer.text_unbounded("a longer string with punctuation: ,.;");

  ByteReader reader(writer.span());
  u8 a = 0;
  u16 b = 0;
  u32 c = 0;
  u64 d = 0;
  std::string e;
  std::string f;
  RG_CHECK(reader.u8(a));
  RG_CHECK(reader.u16(b));
  RG_CHECK(reader.u32(c));
  RG_CHECK(reader.u64(d));
  RG_CHECK(reader.text(16, e));
  RG_CHECK(reader.text_unbounded(4096, f));
  RG_CHECK_EQ(a, 0x12);
  RG_CHECK_EQ(b, 0x3456);
  RG_CHECK_EQ(c, 0x789ABCDEU);
  RG_CHECK_EQ(d, 0x0123456789ABCDEFULL);
  RG_CHECK_EQ(e, std::string("hello"));
  RG_CHECK(reader.fully_consumed());
}

RG_TEST(codec, reader_rejects_truncation_and_oversized_text) {
  ByteWriter writer(16);
  writer.u64(1);
  ByteReader short_reader(writer.span());
  u64 value = 0;
  RG_CHECK(short_reader.u64(value));
  RG_CHECK(!short_reader.u64(value));
  RG_CHECK(!short_reader.ok());

  ByteWriter text_writer(64);
  RG_CHECK(!text_writer.text("0123456789", 4));
  RG_CHECK(text_writer.text("0123", 4));
  ByteReader text_reader(text_writer.span());
  std::string out;
  RG_CHECK(text_reader.text(4, out));
  RG_CHECK_EQ(out, std::string("0123"));

  // A length prefix that exceeds the caller's bound is refused before any
  // allocation is attempted.
  ByteWriter hostile(16);
  hostile.text_unbounded(std::string(1000, 'x'));
  ByteReader hostile_reader(hostile.span());
  std::string hostile_out;
  RG_CHECK(!hostile_reader.text_unbounded(64, hostile_out));
}

RG_TEST(codec, helpers_bound_and_mark_truncation) {
  RG_CHECK(is_ascii_printable("plain text"));
  RG_CHECK(!is_ascii_printable(std::string("bad\x01text", 8)));
  RG_CHECK(is_bounded_text("ok", 4));
  RG_CHECK(!is_bounded_text("toolong", 4));
  RG_CHECK_EQ(truncate_bounded("abcdef", 6), std::string("abcdef"));
  RG_CHECK_EQ(truncate_bounded("abcdef", 4), std::string("a..."));
  RG_CHECK_EQ(truncate_bounded("abcdef", 2), std::string("ab"));
}

// ---------------------------------------------------------------------------
// Wire framing
// ---------------------------------------------------------------------------
RG_TEST(wire, frame_round_trip_preserves_payload) {
  Rng rng(7);
  for (int iteration = 0; iteration < 200; ++iteration) {
    std::vector<std::byte> payload(rng.below(512));
    for (std::byte& byte : payload) {
      byte = static_cast<std::byte>(rng.below(256));
    }
    const Frame frame = make_frame(MessageType::ApplyRequest, payload);
    const std::vector<std::byte> encoded = encode_frame(frame);
    RG_CHECK(!encoded.empty());
    Frame decoded;
    ReasonCode reason = ReasonCode::None;
    RG_CHECK(decode_frame(encoded, decoded, reason));
    RG_CHECK(decoded.type == frame.type);
    RG_CHECK(decoded.payload == payload);
  }
}

RG_TEST(wire, header_rejects_bad_magic_version_and_length) {
  const Frame frame = make_frame(MessageType::HeartbeatRequest, {std::byte{1}});
  std::vector<std::byte> encoded = encode_frame(frame);
  RG_CHECK(!encoded.empty());

  ReasonCode reason = ReasonCode::None;
  FrameHeader header;
  RG_CHECK(decode_frame_header(std::span<const std::byte>(encoded.data(), kWireHeaderSize), header,
                               reason));

  std::vector<std::byte> bad_magic = encoded;
  bad_magic[0] = std::byte{0};
  RG_CHECK(!decode_frame_header(std::span<const std::byte>(bad_magic.data(), kWireHeaderSize),
                                header, reason));
  RG_CHECK(reason == ReasonCode::MalformedInput);

  std::vector<std::byte> bad_version = encoded;
  bad_version[4] = std::byte{9};
  RG_CHECK(!decode_frame_header(std::span<const std::byte>(bad_version.data(), kWireHeaderSize),
                                header, reason));
  RG_CHECK(reason == ReasonCode::UnsupportedOperation);

  std::vector<std::byte> oversized = encoded;
  oversized[12] = std::byte{0xFF};
  oversized[13] = std::byte{0xFF};
  oversized[14] = std::byte{0xFF};
  oversized[15] = std::byte{0xFF};
  RG_CHECK(!decode_frame_header(std::span<const std::byte>(oversized.data(), kWireHeaderSize),
                                header, reason));
  RG_CHECK(reason == ReasonCode::CapacityLimitExceeded);

  // A short header is not read past its end.
  RG_CHECK(!decode_frame_header(std::span<const std::byte>(encoded.data(), 4), header, reason));
}

RG_TEST(wire, body_rejects_truncation_and_corruption) {
  Frame frame = make_frame(MessageType::VerifyResponse, std::vector<std::byte>(100, std::byte{7}));
  const std::vector<std::byte> encoded = encode_frame(frame);
  Frame decoded;
  ReasonCode reason = ReasonCode::None;
  RG_CHECK(decode_frame(encoded, decoded, reason));
  RG_CHECK(!decode_frame(std::span<const std::byte>(encoded.data(), encoded.size() - 1), decoded,
                         reason));

  // Flipping any single bit inside the payload must invalidate the frame.
  for (usize index = kWireHeaderSize; index < kWireHeaderSize + 100; ++index) {
    std::vector<std::byte> mutated = encoded;
    mutated[index] = static_cast<std::byte>(std::to_integer<unsigned>(mutated[index]) ^ 0x01U);
    RG_CHECK(!decode_frame(mutated, decoded, reason));
  }
  // Trailing bytes beyond the frame are ignored by the body decoder.
  std::vector<std::byte> padded = encoded;
  padded.push_back(std::byte{0xAB});
  RG_CHECK(decode_frame(padded, decoded, reason));
}

RG_TEST(wire, messages_round_trip) {
  HelloMessage hello;
  hello.backend = BackendId(3);
  hello.backend_generation = Generation(2);
  hello.boot = WorkerBootId(11);
  hello.epoch = FabricEpoch(5);
  hello.kind = BackendKind::Synthetic;
  hello.capabilities = 0x1F;
  hello.verification = VerificationMode::PostApplyReadback;
  hello.max_rate_ups = 1000;
  hello.max_burst_tokens = 2000;
  hello.label = "worker";
  hello.build = "build";
  HelloMessage hello_out;
  RG_CHECK(decode_message(encode_message(hello), hello_out));
  RG_CHECK(hello_out.backend == hello.backend && hello_out.boot == hello.boot &&
           hello_out.epoch == hello.epoch && hello_out.kind == hello.kind &&
           hello_out.label == hello.label);

  ApplyRequestMessage apply;
  apply.attempt = EnforcementAttemptId(9);
  apply.envelope = RateEnvelopeId(4);
  apply.envelope_generation = Generation(1);
  apply.rate_ups = 1234;
  apply.burst_tokens = 5678;
  apply.refill = RefillSemantics::ContinuousTokenBucket;
  apply.epoch = FabricEpoch(2);
  apply.boot = WorkerBootId(3);
  apply.deadline_ns = 99;
  ApplyRequestMessage apply_out;
  RG_CHECK(decode_message(encode_message(apply), apply_out));
  RG_CHECK(apply_out.attempt == apply.attempt && apply_out.rate_ups == apply.rate_ups &&
           apply_out.burst_tokens == apply.burst_tokens && apply_out.deadline_ns == 99);

  ApplyResponseMessage apply_response;
  apply_response.attempt = EnforcementAttemptId(9);
  apply_response.accepted = true;
  apply_response.reason = ReasonCode::ApplyAcknowledged;
  apply_response.applied_rate_ups = 1234;
  apply_response.applied_burst_tokens = 5678;
  apply_response.epoch = FabricEpoch(2);
  apply_response.boot = WorkerBootId(3);
  apply_response.detail = "ok";
  ApplyResponseMessage apply_response_out;
  RG_CHECK(decode_message(encode_message(apply_response), apply_response_out));
  RG_CHECK(apply_response_out.accepted && apply_response_out.applied_rate_ups == 1234);

  RevokeRequestMessage revoke;
  revoke.attempt = EnforcementAttemptId(10);
  revoke.envelope = RateEnvelopeId(4);
  revoke.envelope_generation = Generation(1);
  revoke.epoch = FabricEpoch(2);
  revoke.boot = WorkerBootId(3);
  revoke.reason = ReasonCode::EnvelopeRevokedByOperator;
  RevokeRequestMessage revoke_out;
  RG_CHECK(decode_message(encode_message(revoke), revoke_out));
  RG_CHECK(revoke_out.reason == revoke.reason && revoke_out.attempt == revoke.attempt);

  RevokeResponseMessage revoke_response;
  revoke_response.attempt = EnforcementAttemptId(10);
  revoke_response.removed = true;
  revoke_response.reason = ReasonCode::RevokeVerified;
  revoke_response.epoch = FabricEpoch(2);
  revoke_response.boot = WorkerBootId(3);
  revoke_response.detail = "gone";
  RevokeResponseMessage revoke_response_out;
  RG_CHECK(decode_message(encode_message(revoke_response), revoke_response_out));
  RG_CHECK(revoke_response_out.removed);

  VerifyRequestMessage verify;
  verify.attempt = EnforcementAttemptId(11);
  verify.envelope = RateEnvelopeId(4);
  verify.envelope_generation = Generation(1);
  verify.epoch = FabricEpoch(2);
  verify.boot = WorkerBootId(3);
  VerifyRequestMessage verify_out;
  RG_CHECK(decode_message(encode_message(verify), verify_out));
  RG_CHECK(verify_out.attempt == verify.attempt);

  VerifyResponseMessage verify_response;
  verify_response.attempt = EnforcementAttemptId(11);
  verify_response.present = true;
  verify_response.observed_rate_ups = 42;
  verify_response.observed_burst_tokens = 43;
  verify_response.epoch = FabricEpoch(2);
  verify_response.boot = WorkerBootId(3);
  verify_response.reason = ReasonCode::ApplyVerified;
  verify_response.detail = "held";
  VerifyResponseMessage verify_response_out;
  RG_CHECK(decode_message(encode_message(verify_response), verify_response_out));
  RG_CHECK(verify_response_out.observed_rate_ups == 42 && verify_response_out.present);

  AccountingResponseMessage accounting;
  accounting.requests_served = 1;
  accounting.applies_applied = 2;
  accounting.revokes_applied = 3;
  accounting.verifications = 4;
  accounting.rejections = 5;
  accounting.malformed_frames = 6;
  accounting.fenced_stale_epoch = 7;
  accounting.fenced_stale_boot = 8;
  accounting.queue_rejections = 9;
  accounting.queue_capacity = 10;
  accounting.queue_high_water = 11;
  accounting.active_envelopes = 12;
  accounting.envelope_limit = 13;
  accounting.idle_noop_requests = 14;
  accounting.boot = WorkerBootId(3);
  accounting.epoch = FabricEpoch(2);
  AccountingResponseMessage accounting_out;
  RG_CHECK(decode_message(encode_message(accounting), accounting_out));
  RG_CHECK(accounting_out.applies_applied == 2 && accounting_out.envelope_limit == 13);

  ErrorMessage error;
  error.reason = ReasonCode::BackendFencedStaleEpoch;
  error.detail = "stale";
  ErrorMessage error_out;
  RG_CHECK(decode_message(encode_message(error), error_out));
  RG_CHECK(error_out.reason == error.reason && error_out.detail == error.detail);
}

RG_TEST(wire, message_decoders_reject_trailing_and_padded_input) {
  HelloMessage hello;
  hello.backend = BackendId(1);
  hello.backend_generation = Generation(1);
  hello.boot = WorkerBootId(1);
  hello.epoch = FabricEpoch(1);
  hello.kind = BackendKind::Synthetic;
  hello.verification = VerificationMode::PostApplyReadback;
  hello.label = "x";
  hello.build = "y";
  std::vector<std::byte> encoded = encode_message(hello);
  RG_CHECK(!encoded.empty());
  encoded.push_back(std::byte{0});
  HelloMessage out;
  RG_CHECK(!decode_message(encoded, out));

  // A text field whose length exceeds the message bound is refused.
  ByteWriter writer(64);
  writer.u64(1);
  writer.u64(1);
  writer.u64(1);
  writer.u64(1);
  writer.u8(static_cast<u8>(BackendKind::Synthetic));
  writer.u8(static_cast<u8>(VerificationMode::PostApplyReadback));
  writer.u16(0);
  writer.u32(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u32(0);
  writer.u32(kWireProtocolVersion);
  writer.u16(static_cast<u16>(kWireMaxTextBytes + 1));
  writer.raw(std::string(kWireMaxTextBytes + 1, 'z'));
  RG_CHECK(!decode_message(writer.span(), out));
}

RG_TEST(wire, adversarial_frames_are_never_accepted) {
  // Deterministic random byte soup: a decoder must refuse all of it, and must
  // never read out of bounds while doing so.
  Rng rng(0xBADF00DULL);
  Frame decoded;
  ReasonCode reason = ReasonCode::None;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::vector<std::byte> bytes(1 + rng.below(64));
    for (std::byte& byte : bytes) {
      byte = static_cast<std::byte>(rng.below(256));
    }
    RG_CHECK(!decode_frame(bytes, decoded, reason));
  }
  // Random mutations of a valid frame are refused unless they leave the frame
  // byte-identical.
  const std::vector<std::byte> valid =
      encode_frame(make_frame(MessageType::ApplyResponse, std::vector<std::byte>(32, std::byte{5})));
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::vector<std::byte> mutated = valid;
    const usize index = static_cast<usize>(rng.below(mutated.size()));
    mutated[index] = static_cast<std::byte>(std::to_integer<unsigned>(mutated[index]) ^
                                            (1U << rng.below(8)));
    if (mutated == valid) {
      continue;
    }
    RG_CHECK(!decode_frame(mutated, decoded, reason));
  }
}

RG_TEST(wire, oversized_frames_are_refused_before_allocation) {
  Frame frame = make_frame(MessageType::ApplyRequest,
                           std::vector<std::byte>(kWireMaxPayloadBytes, std::byte{1}));
  const std::vector<std::byte> encoded = encode_frame(frame);
  RG_CHECK(!encoded.empty());
  Frame decoded;
  ReasonCode reason = ReasonCode::None;
  RG_CHECK(decode_frame(encoded, decoded, reason));

  Frame too_big = make_frame(MessageType::ApplyRequest,
                             std::vector<std::byte>(kWireMaxPayloadBytes + 1, std::byte{1}));
  RG_CHECK(encode_frame(too_big).empty());
}
