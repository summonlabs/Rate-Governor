#ifndef RATE_GOVERNOR_VERSION_HPP
#define RATE_GOVERNOR_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace rate_governor {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProductName = "Rate Governor";
inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs";

// ABI/format revision of the durable journal. Bumped only for incompatible
// on-disk layout changes; readers reject journals they do not understand.
inline constexpr std::uint16_t kJournalFormatVersion = 1;
// Wire protocol revision for the framed backend transport.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_VERSION_HPP
