#include "rate_governor/codec.hpp"

#include <cstdint>

namespace rate_governor {
namespace {

// A plain array, deliberately: the index is always masked to a byte, and a
// plain array keeps that provable to the static analyzer without annotations.
struct Crc32cTable {
  u32 entries[256]{};
};

constexpr Crc32cTable make_crc32c_table() noexcept {
  Crc32cTable table{};
  for (u32 index = 0; index < 256; ++index) {
    u32 crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ kCrc32cPolynomial : (crc >> 1);
    }
    table.entries[index] = crc;
  }
  return table;
}

constexpr Crc32cTable kCrcTable = make_crc32c_table();

}  // namespace

u32 crc32c_extend(u32 seed, std::span<const std::byte> data) noexcept {
  u32 crc = seed ^ 0xFFFFFFFFU;
  for (const std::byte raw : data) {
    const std::uint8_t byte = static_cast<std::uint8_t>(std::to_integer<unsigned int>(raw));
    const u32 index = (crc ^ byte) & 0xFFU;  // always a valid table index
    crc = kCrcTable.entries[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

u32 crc32c(std::span<const std::byte> data) noexcept { return crc32c_extend(0, data); }

// Member definitions spell their parameter types with std::uintN_t: inside the
// class scope the names u8/u16/u32/u64 denote the member functions themselves.
void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::byte>(value & 0xFFU));
  buffer_.push_back(static_cast<std::byte>((value >> 8) & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::raw(std::span<const std::byte> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::raw(std::string_view bytes) {
  const auto* first = reinterpret_cast<const std::byte*>(bytes.data());
  buffer_.insert(buffer_.end(), first, first + bytes.size());
}

bool ByteWriter::text(std::string_view value, usize max_length) {
  if (value.size() > max_length || value.size() > 0xFFFFU) {
    return false;
  }
  u16(static_cast<std::uint16_t>(value.size()));
  raw(value);
  return true;
}

void ByteWriter::text_unbounded(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value);
}

bool ByteReader::need(usize count) noexcept {
  if (failed_) {
    return false;
  }
  if (count > data_.size() - offset_) {
    failed_ = true;
    return false;
  }
  return true;
}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  if (!need(1)) {
    return false;
  }
  out = static_cast<std::uint8_t>(std::to_integer<unsigned int>(data_[offset_]));
  ++offset_;
  return true;
}

bool ByteReader::u16(std::uint16_t& out) noexcept {
  if (!need(2)) {
    return false;
  }
  out = 0;
  for (int index = 0; index < 2; ++index) {
    const auto raw = static_cast<std::uint16_t>(
        std::to_integer<unsigned int>(data_[offset_ + static_cast<usize>(index)]));
    out = static_cast<std::uint16_t>(out | static_cast<std::uint16_t>(raw << (8 * index)));
  }
  offset_ += 2;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  if (!need(4)) {
    return false;
  }
  out = 0;
  for (int index = 0; index < 4; ++index) {
    const auto raw = static_cast<std::uint32_t>(
        std::to_integer<unsigned int>(data_[offset_ + static_cast<usize>(index)]));
    out |= static_cast<std::uint32_t>(raw << (8 * index));
  }
  offset_ += 4;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  if (!need(8)) {
    return false;
  }
  out = 0;
  for (int index = 0; index < 8; ++index) {
    const auto raw = static_cast<std::uint64_t>(
        std::to_integer<unsigned int>(data_[offset_ + static_cast<usize>(index)]));
    out |= static_cast<std::uint64_t>(raw << (8 * index));
  }
  offset_ += 8;
  return true;
}

bool ByteReader::raw(usize count, std::span<const std::byte>& out) noexcept {
  if (!need(count)) {
    return false;
  }
  out = data_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool ByteReader::text(usize max_length, std::string& out) {
  std::uint16_t length = 0;
  if (!u16(length)) {
    return false;
  }
  if (static_cast<usize>(length) > max_length) {
    failed_ = true;
    return false;
  }
  std::span<const std::byte> bytes;
  if (!raw(static_cast<usize>(length), bytes)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool ByteReader::text_unbounded(usize hard_limit, std::string& out) {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (static_cast<usize>(length) > hard_limit) {
    failed_ = true;
    return false;
  }
  std::span<const std::byte> bytes;
  if (!raw(static_cast<usize>(length), bytes)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool ByteReader::skip(usize count) noexcept {
  if (!need(count)) {
    return false;
  }
  offset_ += count;
  return true;
}

bool is_ascii_printable(std::string_view text) noexcept {
  for (const char ch : text) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (value < 0x20U || value > 0x7EU) {
      return false;
    }
  }
  return true;
}

std::string truncate_bounded(std::string text, usize max_length) {
  if (text.size() <= max_length) {
    return text;
  }
  const std::string marker = "...";
  if (max_length <= marker.size()) {
    return text.substr(0, max_length);
  }
  text.resize(max_length - marker.size());
  text.append(marker);
  return text;
}

}  // namespace rate_governor
