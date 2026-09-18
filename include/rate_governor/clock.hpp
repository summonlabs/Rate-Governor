#ifndef RATE_GOVERNOR_CLOCK_HPP
#define RATE_GOVERNOR_CLOCK_HPP

// Time is an explicit input, never an implicit ambient resource.
//
// Every authoritative Rate Governor decision receives its instant from an
// IClock supplied by the embedder. The engine never calls std::chrono directly,
// so a deterministic ManualClock can drive any scenario, and no decision can
// depend on wall-clock skew that the caller did not choose to expose.

#include <atomic>
#include <chrono>
#include <cstdint>

#include "rate_governor/checked_math.hpp"

namespace rate_governor {

using TimestampNs = u64;

// A window upper bound of kNeverExpiresNs means "no scheduled expiry".
inline constexpr TimestampNs kNeverExpiresNs = kU64Max;

[[nodiscard]] constexpr bool is_never(TimestampNs t) noexcept { return t == kNeverExpiresNs; }

class IClock {
 public:
  IClock() = default;
  virtual ~IClock();
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  IClock(IClock&&) = delete;
  IClock& operator=(IClock&&) = delete;

  [[nodiscard]] virtual TimestampNs NowNs() const noexcept = 0;
};

// Monotonic process clock. Note that steady_clock is not comparable across
// process incarnations, which is precisely why it is only ever used for
// durations inside one incarnation.
class SteadyClock final : public IClock {
 public:
  [[nodiscard]] TimestampNs NowNs() const noexcept override {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return ns < 0 ? 0ULL : static_cast<TimestampNs>(ns);
  }
};

// Deterministic clock for tests, replay and offline evaluation.
class ManualClock final : public IClock {
 public:
  explicit ManualClock(TimestampNs start = 0) noexcept : now_(start) {}

  [[nodiscard]] TimestampNs NowNs() const noexcept override {
    return now_.load(std::memory_order_relaxed);
  }

  void set(TimestampNs value) noexcept { now_.store(value, std::memory_order_relaxed); }

  void advance(TimestampNs delta) noexcept {
    now_.store(now_.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
  }

 private:
  std::atomic<TimestampNs> now_;
};

// Half-open validity window: [not_before, invalid_after).
struct ValidityWindow {
  TimestampNs not_before_ns{0};
  TimestampNs invalid_after_ns{kNeverExpiresNs};

  [[nodiscard]] constexpr bool contains(TimestampNs now) const noexcept {
    if (now < not_before_ns) {
      return false;
    }
    if (is_never(invalid_after_ns)) {
      return true;
    }
    return now < invalid_after_ns;
  }

  [[nodiscard]] constexpr bool empty_window() const noexcept {
    return !is_never(invalid_after_ns) && invalid_after_ns <= not_before_ns;
  }
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_CLOCK_HPP
