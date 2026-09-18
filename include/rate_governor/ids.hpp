#ifndef RATE_GOVERNOR_IDS_HPP
#define RATE_GOVERNOR_IDS_HPP

// Strongly typed identities.
//
// Rate Governor never passes a bare integer where an identity is meant. Every
// identity carries a type tag, so a Generation can never be assigned to a
// FlowId, and a ResourceId can never be silently substituted for a GrantId.
// Identities are also text round-trippable ("flow:17") for journals, wire
// frames, explanations and operator tooling.

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "rate_governor/checked_math.hpp"

namespace rate_governor {

// Strict decimal parse: no sign, no whitespace, no overflow, no empty input.
[[nodiscard]] bool parse_u64_strict(std::string_view text, u64& out) noexcept;

// Bounded, allocation-free-ish text helpers.
[[nodiscard]] std::string join_identity(std::string_view prefix, u64 value);

template <class Tag>
struct IdTagTraits;

template <class Tag, class T = u64>
class StrongId {
 public:
  using tag_type = Tag;
  using value_type = T;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(T value) noexcept : value_(value) {}

  [[nodiscard]] constexpr T value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != T{0}; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const StrongId&, const StrongId&) noexcept = default;

  [[nodiscard]] std::string to_string() const {
    return join_identity(IdTagTraits<Tag>::prefix, static_cast<u64>(value_));
  }

  [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept {
    const std::string_view prefix = IdTagTraits<Tag>::prefix;
    if (text.size() <= prefix.size() + 1) {
      return std::nullopt;
    }
    if (text.substr(0, prefix.size()) != prefix || text[prefix.size()] != ':') {
      return std::nullopt;
    }
    u64 parsed = 0;
    if (!parse_u64_strict(text.substr(prefix.size() + 1), parsed)) {
      return std::nullopt;
    }
    return StrongId(static_cast<T>(parsed));
  }

 private:
  T value_{0};
};

#define RATE_GOVERNOR_DEFINE_ID(type_name, prefix_text)              \
  struct type_name##Tag {};                                         \
  template <>                                                       \
  struct IdTagTraits<type_name##Tag> {                              \
    static constexpr std::string_view prefix = prefix_text;         \
  };                                                                \
  using type_name = StrongId<type_name##Tag>

RATE_GOVERNOR_DEFINE_ID(RateEnvelopeId, "env");
RATE_GOVERNOR_DEFINE_ID(FlowId, "flow");
RATE_GOVERNOR_DEFINE_ID(ResourceId, "res");
RATE_GOVERNOR_DEFINE_ID(GrantId, "grant");
RATE_GOVERNOR_DEFINE_ID(ReservationId, "rsv");
RATE_GOVERNOR_DEFINE_ID(PolicyId, "pol");
RATE_GOVERNOR_DEFINE_ID(BackendId, "bkd");
RATE_GOVERNOR_DEFINE_ID(EnforcementAttemptId, "att");
RATE_GOVERNOR_DEFINE_ID(Generation, "gen");
RATE_GOVERNOR_DEFINE_ID(FabricEpoch, "epoch");
RATE_GOVERNOR_DEFINE_ID(WorkerBootId, "boot");
RATE_GOVERNOR_DEFINE_ID(AuditSequence, "seq");

#undef RATE_GOVERNOR_DEFINE_ID

// Monotonic generation helper. A generation is bumped whenever the meaning of
// the referenced authority changes, which is exactly what invalidates stale
// plans, attempts and completions.
class GenerationCounter {
 public:
  constexpr GenerationCounter() noexcept = default;
  explicit constexpr GenerationCounter(Generation initial) noexcept : current_(initial) {}

  [[nodiscard]] constexpr Generation current() const noexcept { return current_; }

  [[nodiscard]] constexpr Generation next() noexcept {
    const u64 value = current_.value();
    current_ = Generation(value == kU64Max ? kU64Max : value + 1);
    return current_;
  }

  constexpr void observe(Generation seen) noexcept {
    if (seen.value() > current_.value()) {
      current_ = seen;
    }
  }

 private:
  Generation current_{Generation(1)};
};

// Deterministic 64-bit mixing used for identity-derived decisions that must not
// depend on address space layout or process incarnation.
[[nodiscard]] constexpr u64 splitmix64(u64& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

[[nodiscard]] constexpr u64 mix64(u64 value) noexcept {
  u64 state = value;
  return splitmix64(state);
}

}  // namespace rate_governor

namespace std {

template <class Tag, class T>
struct hash<rate_governor::StrongId<Tag, T>> {
  [[nodiscard]] size_t operator()(const rate_governor::StrongId<Tag, T>& id) const noexcept {
    return static_cast<size_t>(rate_governor::mix64(static_cast<rate_governor::u64>(id.value())));
  }
};

}  // namespace std

#endif  // RATE_GOVERNOR_IDS_HPP
