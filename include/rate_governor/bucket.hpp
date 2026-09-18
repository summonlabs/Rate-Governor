#ifndef RATE_GOVERNOR_BUCKET_HPP
#define RATE_GOVERNOR_BUCKET_HPP

// Exact token bucket accounting.
//
// The bucket keeps an exact fractional remainder, expressed as a numerator over
// kNanosecondsPerSecond, so that refilling 1 ns a thousand times yields exactly
// the same token count as refilling 1000 ns once. Token counts are integers,
// never overflow, and can never go negative: a consume that cannot be satisfied
// is refused, it does not borrow.

#include "rate_governor/checked_math.hpp"
#include "rate_governor/clock.hpp"

namespace rate_governor {

struct TokenBucket {
  u64 tokens{0};
  u64 capacity{0};
  u64 rate_ups{0};
  TimestampNs last_refill_ns{0};
  // Numerator of the carried fractional token, always < kNanosecondsPerSecond.
  u64 fractional_numerator{0};
  bool initialized{false};
};

struct RefillOutcome {
  bool clock_regressed{false};
  bool saturated{false};
  u64 tokens_granted{0};
  u64 tokens_available{0};
  TimestampNs elapsed_ns{0};
};

// Installs capacity/rate and starts the bucket full. A capacity of zero is
// legal and means every consume is refused.
void configure_bucket(TokenBucket& bucket, u64 capacity, u64 rate_ups, TimestampNs now) noexcept;

// Advances the bucket to "now". Elapsed time is computed with checked
// arithmetic: a regression is reported and treated as zero elapsed time (never
// as negative credit), and a backwards jump never rewinds last_refill_ns.
[[nodiscard]] RefillOutcome refill_bucket(TokenBucket& bucket, TimestampNs now) noexcept;

// Removes "amount" tokens if available. Never yields a negative balance.
[[nodiscard]] bool try_consume(TokenBucket& bucket, u64 amount) noexcept;

// Reduces capacity; if the bucket holds more than the new capacity the surplus
// is dropped. Capacity can also grow, but a capacity change never grants
// tokens by itself.
void set_bucket_capacity(TokenBucket& bucket, u64 capacity) noexcept;

// Changes the refill rate after first accounting all elapsed time at the old
// rate, so a rate change cannot retroactively re-price the past.
void set_bucket_rate(TokenBucket& bucket, u64 rate_ups, TimestampNs now) noexcept;

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_BUCKET_HPP
