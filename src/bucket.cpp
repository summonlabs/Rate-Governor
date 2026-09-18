#include "rate_governor/bucket.hpp"

namespace rate_governor {

void configure_bucket(TokenBucket& bucket, u64 capacity, u64 rate_ups, TimestampNs now) noexcept {
  bucket.capacity = capacity;
  bucket.rate_ups = rate_ups;
  bucket.tokens = capacity;
  bucket.last_refill_ns = now;
  bucket.fractional_numerator = 0;
  bucket.initialized = true;
}

RefillOutcome refill_bucket(TokenBucket& bucket, TimestampNs now) noexcept {
  RefillOutcome outcome;
  if (!bucket.initialized) {
    bucket.initialized = true;
    bucket.last_refill_ns = now;
    bucket.tokens = bucket.tokens > bucket.capacity ? bucket.capacity : bucket.tokens;
    outcome.tokens_available = bucket.tokens;
    return outcome;
  }

  if (now < bucket.last_refill_ns) {
    // A backwards clock never mints tokens and never moves the anchor. Moving
    // it would re-price the interval [now, last_refill] a second time once the
    // clock caught up, which is exactly how a clock glitch becomes free burst.
    outcome.clock_regressed = true;
    outcome.tokens_available = bucket.tokens;
    return outcome;
  }

  const TimestampNs elapsed = now - bucket.last_refill_ns;
  outcome.elapsed_ns = elapsed;
  bucket.last_refill_ns = now;

  if (elapsed == 0 || bucket.rate_ups == 0 || bucket.capacity == 0) {
    if (bucket.tokens > bucket.capacity) {
      bucket.tokens = bucket.capacity;
      outcome.saturated = true;
    }
    outcome.tokens_available = bucket.tokens;
    return outcome;
  }

  const u64 before = bucket.tokens;
  const MulDivMod scaled = mul_div_mod(elapsed, bucket.rate_ups, kNanosecondsPerSecond);
  if (!scaled.ok) {
    // The product cannot be expressed at this granularity; the only safe
    // reading is "at least a full bucket".
    bucket.tokens = bucket.capacity;
    bucket.fractional_numerator = 0;
    outcome.saturated = true;
    outcome.tokens_granted = bucket.capacity - before;
    outcome.tokens_available = bucket.capacity;
    return outcome;
  }

  u64 granted = scaled.quotient;
  const u64 carry = bucket.fractional_numerator + scaled.remainder;  // both < 1e9
  granted = saturating_add(granted, carry / kNanosecondsPerSecond);
  bucket.fractional_numerator = carry % kNanosecondsPerSecond;

  bucket.tokens = saturating_add(before, granted);
  if (bucket.tokens > bucket.capacity) {
    bucket.tokens = bucket.capacity;
    outcome.saturated = true;
  }
  // "Granted" is what the balance actually received, not what was computed
  // before the ceiling clipped it.
  outcome.tokens_granted = bucket.tokens - before;
  outcome.tokens_available = bucket.tokens;
  return outcome;
}

bool try_consume(TokenBucket& bucket, u64 amount) noexcept {
  if (amount > bucket.tokens) {
    return false;
  }
  bucket.tokens -= amount;
  return true;
}

void set_bucket_capacity(TokenBucket& bucket, u64 capacity) noexcept {
  bucket.capacity = capacity;
  if (bucket.tokens > capacity) {
    bucket.tokens = capacity;
  }
}

void set_bucket_rate(TokenBucket& bucket, u64 rate_ups, TimestampNs now) noexcept {
  if (bucket.initialized && now >= bucket.last_refill_ns) {
    (void)refill_bucket(bucket, now);
  }
  bucket.rate_ups = rate_ups;
}

}  // namespace rate_governor
