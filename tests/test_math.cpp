#include "test_support.hpp"

#include <limits>

using namespace rate_governor;
using namespace rgtest;  // NOLINT(google-build-using-namespace)

namespace {

// Exact reference: quotient * divisor + remainder must reproduce the 128-bit
// product, and the remainder must be smaller than the divisor.
bool division_identity_holds(u64 a, u64 b, u64 d) {
  if (d == 0) {
    return true;
  }
  const MulDivMod result = mul_div_mod(a, b, d);
  if (!result.ok) {
    return false;
  }
  const U128 product = mul_wide(a, b);
  const U128 scaled = mul_wide(result.quotient, d);
  U128 sum;
  sum.lo = scaled.lo + result.remainder;
  sum.hi = scaled.hi + (sum.lo < scaled.lo ? 1ULL : 0ULL);
  return sum == product && result.remainder < d;
}

bool quotient_overflow_expected(u64 a, u64 b, u64 d) {
  if (d == 0) {
    return true;
  }
  return mul_wide(a, b).hi >= d;
}

}  // namespace

RG_TEST(math, mul_wide_matches_known_products) {
  RG_CHECK(mul_wide(0, 0).hi == 0 && mul_wide(0, 0).lo == 0);
  RG_CHECK(mul_wide(1, 1).hi == 0 && mul_wide(1, 1).lo == 1);
  const U128 mid = mul_wide(0xFFFFFFFFULL, 0xFFFFFFFFULL);
  RG_CHECK(mid.hi == 0 && mid.lo == 0xFFFFFFFE00000001ULL);
  const U128 max_product = mul_wide(kU64Max, kU64Max);
  // (2^64 - 1)^2 = 2^128 - 2^65 + 1
  RG_CHECK_EQ(max_product.hi, 0xFFFFFFFFFFFFFFFEULL);
  RG_CHECK_EQ(max_product.lo, 0x0000000000000001ULL);
}

RG_TEST(math, mul_div_is_exact_over_seeded_triples) {
  Rng rng(0x5EED1234ULL);
  for (int index = 0; index < 20000; ++index) {
    const u64 a = rng.next();
    const u64 b = rng.next();
    u64 d = rng.next();
    if (d == 0) {
      d = 1;
    }
    const MulDivMod result = mul_div_mod(a, b, d);
    const bool expected_overflow = quotient_overflow_expected(a, b, d);
    RG_CHECK_EQ(result.ok, !expected_overflow);
    if (result.ok) {
      RG_CHECK(division_identity_holds(a, b, d));
    }
    RG_CHECK(!mul_div_mod(a, b, 0).ok);
  }
}

RG_TEST(math, mul_div_matches_small_exhaustive) {
  for (u64 a = 0; a < 40; ++a) {
    for (u64 b = 0; b < 40; ++b) {
      for (u64 d = 1; d < 40; ++d) {
        u64 out = 0;
        RG_CHECK(mul_div(a, b, d, out));
        RG_CHECK_EQ(out, (a * b) / d);
        u64 remainder = 0;
        RG_CHECK(mul_mod(a, b, d, remainder));
        RG_CHECK_EQ(remainder, (a * b) % d);
      }
    }
  }
}

RG_TEST(math, mul_div_boundary_values) {
  u64 out = 0;
  RG_CHECK(mul_div(1000000000ULL, 3, 1000000000ULL, out));
  RG_CHECK_EQ(out, 3ULL);
  RG_CHECK(mul_div(kU64Max, 1, 1, out));
  RG_CHECK_EQ(out, kU64Max);
  RG_CHECK(!mul_div(kU64Max, 2, 1, out));
  RG_CHECK(!mul_div(kU64Max, kU64Max, 1, out));
  RG_CHECK(mul_div(kU64Max, kU64Max, kU64Max, out));
  RG_CHECK_EQ(out, kU64Max);
  // (2^64 - 1)^2 / 2 does not fit in 64 bits: it must be reported, not wrapped.
  const MulDivMod precise = mul_div_mod(kU64Max, kU64Max, 2);
  RG_CHECK(!precise.ok);
  RG_CHECK(mul_div_mod(kU64Max, 2, 3).ok);
  RG_CHECK(division_identity_holds(kU64Max, 2, 3));
  RG_CHECK(division_identity_holds(kU64Max, 1000000007ULL, kU64Max));
  RG_CHECK(division_identity_holds(1000000007ULL, kU64Max, kU64Max));
}

RG_TEST(math, checked_arithmetic_reports_overflow) {
  u64 out = 0;
  RG_CHECK(checked_add(1, 2, out));
  RG_CHECK_EQ(out, 3ULL);
  RG_CHECK(!checked_add(kU64Max, 1, out));
  RG_CHECK(checked_sub(5, 3, out));
  RG_CHECK_EQ(out, 2ULL);
  RG_CHECK(!checked_sub(0, 1, out));
  RG_CHECK(checked_mul(1000, 1000, out));
  RG_CHECK_EQ(out, 1000000ULL);
  RG_CHECK(!checked_mul(kU64Max, 2, out));
  RG_CHECK_EQ(saturating_add(kU64Max, 10), kU64Max);
  RG_CHECK_EQ(saturating_mul(kU64Max, kU64Max), kU64Max);
  RG_CHECK_EQ(saturating_sub(1, 2), 0ULL);
  RG_CHECK_EQ(saturating_mul_div(kU64Max, kU64Max, 1), kU64Max);
  RG_CHECK_EQ(clamp_u64(5, 1, 3), 3ULL);
  RG_CHECK_EQ(clamp_u64(0, 1, 3), 1ULL);
  RG_CHECK_EQ(clamp_u64(2, 1, 3), 2ULL);
  RG_CHECK_EQ(gcd(12, 18), 6ULL);
}

RG_TEST(math, wide_division_rejects_impossible_quotients) {
  DivModWide result = divmod_wide(U128{1, 0}, 1);
  RG_CHECK(!result.ok);
  result = divmod_wide(U128{0, 100}, 7);
  RG_CHECK(result.ok);
  RG_CHECK_EQ(result.quotient, 14ULL);
  RG_CHECK_EQ(result.remainder, 2ULL);
  result = divmod_wide(U128{0, 5}, 0);
  RG_CHECK(!result.ok);
}

RG_TEST(math, splitmix_is_deterministic_and_mixes) {
  u64 a = 42;
  u64 b = 42;
  const u64 first = splitmix64(a);
  const u64 second = splitmix64(b);
  RG_CHECK_EQ(first, second);
  RG_CHECK(first != splitmix64(a));
  RG_CHECK(mix64(1) != mix64(2));
  RG_CHECK_EQ(mix64(7), mix64(7));
}

// ---------------------------------------------------------------------------
// Token bucket
// ---------------------------------------------------------------------------
RG_TEST(bucket, configure_and_consume_never_goes_negative) {
  TokenBucket bucket;
  configure_bucket(bucket, 100, 50, 0);
  RG_CHECK_EQ(bucket.tokens, 100ULL);
  RG_CHECK(try_consume(bucket, 100));
  RG_CHECK_EQ(bucket.tokens, 0ULL);
  RG_CHECK(!try_consume(bucket, 1));
  RG_CHECK_EQ(bucket.tokens, 0ULL);
  RG_CHECK(try_consume(bucket, 0));
  // A zero-capacity bucket refuses everything, including zero tokens above the
  // empty balance.
  TokenBucket empty;
  configure_bucket(empty, 0, 1000, 0);
  RG_CHECK(!try_consume(empty, 1));
  const RefillOutcome outcome = refill_bucket(empty, 1000000000ULL);
  RG_CHECK_EQ(outcome.tokens_available, 0ULL);
}

RG_TEST(bucket, refill_is_exact_and_carries_fractions) {
  TokenBucket bucket;
  configure_bucket(bucket, 10, 1, 0);  // one token per second
  RG_CHECK(try_consume(bucket, 10));
  RefillOutcome outcome = refill_bucket(bucket, 999999999ULL);
  RG_CHECK_EQ(outcome.tokens_available, 0ULL);
  outcome = refill_bucket(bucket, 1000000000ULL);
  RG_CHECK_EQ(outcome.tokens_available, 1ULL);
  RG_CHECK_EQ(outcome.tokens_granted, 1ULL);
}

RG_TEST(bucket, split_intervals_agree_with_one_interval) {
  Rng rng(0xC0FFEEULL);
  for (int iteration = 0; iteration < 500; ++iteration) {
    const u64 rate = 1 + rng.below(100000);
    const u64 capacity = 1000000;
    TokenBucket whole;
    configure_bucket(whole, capacity, rate, 0);
    RG_CHECK(try_consume(whole, capacity));
    TokenBucket split;
    configure_bucket(split, capacity, rate, 0);
    RG_CHECK(try_consume(split, capacity));

    TimestampNs now = 0;
    const u64 total = rng.below(2000000000ULL);
    u64 remaining = total;
    while (remaining > 0) {
      const u64 step = 1 + rng.below(std::min<u64>(remaining, 1000000ULL));
      now += step;
      remaining -= step;
      (void)refill_bucket(split, now);
    }
    now = total;
    (void)refill_bucket(whole, now);
    // Neither bucket may reach capacity here: total * rate / 1e9 is far below
    // the million-token capacity.
    RG_CHECK_EQ(split.tokens, whole.tokens);
  }
}

RG_TEST(bucket, backwards_clock_grants_nothing_and_keeps_the_anchor) {
  TokenBucket bucket;
  configure_bucket(bucket, 1000, 1000, 1000000000ULL);
  RG_CHECK(try_consume(bucket, 1000));
  const RefillOutcome regressed = refill_bucket(bucket, 500000000ULL);
  RG_CHECK(regressed.clock_regressed);
  RG_CHECK_EQ(bucket.tokens, 0ULL);
  RG_CHECK_EQ(bucket.last_refill_ns, 1000000000ULL);
  // Once the clock catches up, the interval is priced exactly once.
  const RefillOutcome forward = refill_bucket(bucket, 1500000000ULL);
  RG_CHECK(!forward.clock_regressed);
  RG_CHECK_EQ(forward.elapsed_ns, 500000000ULL);
  RG_CHECK_EQ(bucket.tokens, 500ULL);
}

RG_TEST(bucket, saturates_without_overflow_on_absurd_inputs) {
  TokenBucket bucket;
  configure_bucket(bucket, 1000, kU64Max, 0);
  RG_CHECK(try_consume(bucket, 1000));
  const RefillOutcome outcome = refill_bucket(bucket, kU64Max);
  RG_CHECK(outcome.saturated);
  RG_CHECK_EQ(bucket.tokens, 1000ULL);
  RG_CHECK(bucket.tokens <= bucket.capacity);
}

RG_TEST(bucket, capacity_change_clamps_and_rate_change_reprices) {
  TokenBucket bucket;
  configure_bucket(bucket, 1000, 1000, 0);
  set_bucket_capacity(bucket, 100);
  RG_CHECK_EQ(bucket.tokens, 100ULL);
  set_bucket_capacity(bucket, 500);
  RG_CHECK_EQ(bucket.tokens, 100ULL);
  RG_CHECK(try_consume(bucket, 100));
  set_bucket_rate(bucket, 1000000, 0);
  const RefillOutcome outcome = refill_bucket(bucket, 1000000000ULL);
  RG_CHECK_EQ(outcome.tokens_granted, 500ULL);  // clamped to the new capacity
  RG_CHECK_EQ(bucket.tokens, 500ULL);
}

RG_TEST(bucket, uninitialized_refill_initializes_without_granting) {
  TokenBucket bucket;
  bucket.capacity = 50;
  bucket.rate_ups = 10;
  const RefillOutcome outcome = refill_bucket(bucket, 12345);
  RG_CHECK(bucket.initialized);
  RG_CHECK_EQ(outcome.tokens_granted, 0ULL);
  RG_CHECK_EQ(bucket.last_refill_ns, 12345ULL);
}

// ---------------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------------
RG_TEST(reason, vocabulary_is_complete_and_classified) {
  // Ranges that every defined ReasonCode falls into. Enumerating them here
  // means a new enumerator without a name or a class fails this test.
  const std::vector<std::pair<int, int>> ranges = {
      {0, 32}, {40, 62}, {70, 93}, {100, 104}, {110, 115}, {120, 125}, {130, 135},
  };
  for (const auto& range : ranges) {
    for (int value = range.first; value <= range.second; ++value) {
      const auto code = static_cast<ReasonCode>(value);
      RG_CHECK(!to_string_view(code).empty());
      RG_CHECK(to_string_view(code) != std::string_view("Unknown"));
      if (code == ReasonCode::None) {
        RG_CHECK(classify(code) == ReasonClass::None);
      } else if (code == ReasonCode::UnknownAuthority) {
        RG_CHECK(classify(code) == ReasonClass::UnknownAuthority);
      } else {
        RG_CHECK(classify(code) != ReasonClass::None);
        RG_CHECK(classify(code) != ReasonClass::UnknownAuthority);
      }
    }
  }
  RG_CHECK(is_denial(ReasonCode::PlanRejectedCeilingZero));
  RG_CHECK(!is_denial(ReasonCode::ApplyVerified));
  RG_CHECK(is_staleness(ReasonCode::EnvelopeStaleGrantRecalled));
  RG_CHECK(is_fencing(ReasonCode::AttemptFencedByEpoch));
  RG_CHECK(!is_fencing(ReasonCode::ApplyVerified));
}

RG_TEST(reason, ids_round_trip_and_reject_junk) {
  const RateEnvelopeId envelope(42);
  RG_CHECK_EQ(envelope.to_string(), std::string("env:42"));
  const auto parsed = RateEnvelopeId::parse("env:42");
  RG_CHECK(parsed.has_value());
  RG_CHECK(parsed.value() == envelope);
  RG_CHECK(!RateEnvelopeId::parse("flow:42").has_value());
  RG_CHECK(!RateEnvelopeId::parse("env:").has_value());
  RG_CHECK(!RateEnvelopeId::parse("env:-1").has_value());
  RG_CHECK(!RateEnvelopeId::parse("env:+1").has_value());
  RG_CHECK(!RateEnvelopeId::parse("env:1x").has_value());
  RG_CHECK(!RateEnvelopeId::parse("env:99999999999999999999999").has_value());
  RG_CHECK(!RateEnvelopeId::parse("").has_value());
  RG_CHECK_EQ(Generation(7).to_string(), std::string("gen:7"));
  RG_CHECK_EQ(FabricEpoch(3).to_string(), std::string("epoch:3"));
  RG_CHECK_EQ(WorkerBootId(9).to_string(), std::string("boot:9"));
  u64 ignored = 0;
  RG_CHECK(!parse_u64_strict("18446744073709551616", ignored));
  u64 value = 0;
  RG_CHECK(parse_u64_strict("18446744073709551615", value));
  RG_CHECK_EQ(value, kU64Max);
}

RG_TEST(reason, generation_counter_is_monotonic) {
  GenerationCounter counter(Generation(5));
  RG_CHECK_EQ(counter.current().value(), 5ULL);
  RG_CHECK_EQ(counter.next().value(), 6ULL);
  counter.observe(Generation(10));
  RG_CHECK_EQ(counter.current().value(), 10ULL);
  counter.observe(Generation(3));
  RG_CHECK_EQ(counter.current().value(), 10ULL);
}

RG_TEST(reason, envelope_state_classification) {
  RG_CHECK(state_holds_enforceable_effect(EnvelopeState::Applied));
  RG_CHECK(state_holds_enforceable_effect(EnvelopeState::Degraded));
  RG_CHECK(!state_holds_enforceable_effect(EnvelopeState::Revoked));
  RG_CHECK(!state_holds_enforceable_effect(EnvelopeState::Stale));
  RG_CHECK(attempt_state_may_publish_success(AttemptState::Verified));
  RG_CHECK(!attempt_state_may_publish_success(AttemptState::Acknowledged));
  RG_CHECK(!attempt_state_may_publish_success(AttemptState::Ambiguous));
  RG_CHECK(!attempt_state_may_publish_success(AttemptState::Cancelled));
  RG_CHECK(!attempt_state_may_publish_success(AttemptState::LateRejected));
  RG_CHECK(attempt_state_is_terminal(AttemptState::Abandoned));
  RG_CHECK(!attempt_state_is_terminal(AttemptState::Dispatched));
}
