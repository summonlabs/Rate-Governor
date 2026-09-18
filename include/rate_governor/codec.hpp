#ifndef RATE_GOVERNOR_CODEC_HPP
#define RATE_GOVERNOR_CODEC_HPP

// Deterministic little-endian binary codec shared by the durable journal and
// the backend transport. Every read is bounds checked, every length is
// bounded, and every failure is reported instead of being papered over.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rate_governor/checked_math.hpp"

namespace rate_governor {

// CRC-32C (Castagnoli, reflected, polynomial 0x1EDC6F41). Software
// implementation; no hardware acceleration is claimed.
inline constexpr u32 kCrc32cPolynomial = 0x82F63B78U;

[[nodiscard]] u32 crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] u32 crc32c_extend(u32 seed, std::span<const std::byte> data) noexcept;

class ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(usize reserve) { buffer_.reserve(reserve); }

  void u8(u8 value);
  void u16(u16 value);
  void u32(u32 value);
  void u64(u64 value);
  void raw(std::span<const std::byte> bytes);
  void raw(std::string_view bytes);
  // Bounded text: rejects text longer than max_length instead of truncating.
  [[nodiscard]] bool text(std::string_view value, usize max_length);
  // Unbounded-length text with an explicit u32 length prefix; the caller is
  // responsible for keeping the total frame inside the configured bound.
  void text_unbounded(std::string_view value);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::vector<std::byte> buffer_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] bool u8(u8& out) noexcept;
  [[nodiscard]] bool u16(u16& out) noexcept;
  [[nodiscard]] bool u32(u32& out) noexcept;
  [[nodiscard]] bool u64(u64& out) noexcept;
  [[nodiscard]] bool raw(usize count, std::span<const std::byte>& out) noexcept;
  [[nodiscard]] bool text(usize max_length, std::string& out);
  [[nodiscard]] bool text_unbounded(usize hard_limit, std::string& out);
  [[nodiscard]] bool skip(usize count) noexcept;

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] bool empty() const noexcept { return offset_ >= data_.size(); }
  [[nodiscard]] usize remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] usize offset() const noexcept { return offset_; }
  // A well-formed record consumes its payload exactly.
  [[nodiscard]] bool fully_consumed() const noexcept { return offset_ == data_.size(); }

 private:
  [[nodiscard]] bool need(usize count) noexcept;

  std::span<const std::byte> data_;
  usize offset_{0};
  bool failed_{false};
};

// Strict helpers used by both journal and wire decoding.
[[nodiscard]] bool is_ascii_printable(std::string_view text) noexcept;

// Bounded text for explanations and audit detail. Truncation is always marked,
// so a truncated explanation can never be mistaken for a complete one.
[[nodiscard]] std::string truncate_bounded(std::string text, usize max_length);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_CODEC_HPP
