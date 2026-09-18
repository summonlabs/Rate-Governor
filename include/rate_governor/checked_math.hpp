#ifndef RATE_GOVERNOR_CHECKED_MATH_HPP
#define RATE_GOVERNOR_CHECKED_MATH_HPP

// Exact integer arithmetic primitives.
//
// Every rate, burst, duration and token figure in Rate Governor is an integer.
// No floating point value ever participates in an authoritative decision. The
// helpers below are the only sanctioned way to combine externally influenced
// magnitudes: they either produce an exact result or report failure, and they
// never silently wrap.

#include <cstdint>
#include <limits>

namespace rate_governor {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using usize = std::size_t;

inline constexpr u64 kU64Max = std::numeric_limits<u64>::max();
inline constexpr u64 kNanosecondsPerSecond = 1000000000ULL;

// 128-bit product held as two 64-bit halves.
struct U128 {
  u64 hi{0};
  u64 lo{0};

  friend constexpr bool operator==(const U128& a, const U128& b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
};

// Exact 64x64 -> 128 widening multiply.
[[nodiscard]] constexpr U128 mul_wide(u64 a, u64 b) noexcept {
  const u64 a_lo = a & 0xFFFFFFFFULL;
  const u64 a_hi = a >> 32;
  const u64 b_lo = b & 0xFFFFFFFFULL;
  const u64 b_hi = b >> 32;

  const u64 p0 = a_lo * b_lo;
  const u64 p1 = a_lo * b_hi;
  const u64 p2 = a_hi * b_lo;
  const u64 p3 = a_hi * b_hi;

  const u64 mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);

  U128 out;
  out.lo = (mid << 32) | (p0 & 0xFFFFFFFFULL);
  out.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  return out;
}

[[nodiscard]] constexpr int compare_wide(const U128& a, const U128& b) noexcept {
  if (a.hi != b.hi) {
    return a.hi < b.hi ? -1 : 1;
  }
  if (a.lo != b.lo) {
    return a.lo < b.lo ? -1 : 1;
  }
  return 0;
}

struct DivModWide {
  bool ok{false};   // false when the divisor is zero or the quotient is >= 2^64
  u64 quotient{0};
  u64 remainder{0};
};

// Exact floor division of a 128-bit value by a 64-bit divisor.
// Reports overflow instead of wrapping when the quotient does not fit in 64 bits.
[[nodiscard]] constexpr DivModWide divmod_wide(const U128& value, u64 divisor) noexcept {
  DivModWide out;
  if (divisor == 0) {
    return out;
  }
  if (value.hi >= divisor) {
    return out;  // quotient >= 2^64
  }
  u64 remainder = value.hi;
  u64 quotient = 0;
  for (int bit = 63; bit >= 0; --bit) {
    const u64 carry = remainder >> 63;
    remainder = (remainder << 1) | ((value.lo >> bit) & 1ULL);
    if (carry != 0 || remainder >= divisor) {
      remainder -= divisor;
      quotient |= (1ULL << bit);
    }
  }
  out.ok = true;
  out.quotient = quotient;
  out.remainder = remainder;
  return out;
}

// Exact (a * b) mod m without any 128-bit division overflow constraint.
[[nodiscard]] constexpr bool mul_mod(u64 a, u64 b, u64 m, u64& out) noexcept {
  if (m == 0) {
    return false;
  }
  const U128 product = mul_wide(a, b);
  u64 remainder = product.hi % m;
  for (int bit = 63; bit >= 0; --bit) {
    const u64 carry = remainder >> 63;
    remainder = (remainder << 1) | ((product.lo >> bit) & 1ULL);
    if (carry != 0 || remainder >= m) {
      remainder -= m;
    }
  }
  out = remainder;
  return true;
}

struct MulDivMod {
  bool ok{false};
  u64 quotient{0};
  u64 remainder{0};
};

// Exact floor(a * b / d) with the exact remainder, computed in 128 bits.
[[nodiscard]] constexpr MulDivMod mul_div_mod(u64 a, u64 b, u64 d) noexcept {
  MulDivMod out;
  if (d == 0) {
    return out;
  }
  const DivModWide dm = divmod_wide(mul_wide(a, b), d);
  if (!dm.ok) {
    return out;
  }
  out.ok = true;
  out.quotient = dm.quotient;
  out.remainder = dm.remainder;
  return out;
}

[[nodiscard]] constexpr bool mul_div(u64 a, u64 b, u64 d, u64& out) noexcept {
  const MulDivMod r = mul_div_mod(a, b, d);
  if (!r.ok) {
    return false;
  }
  out = r.quotient;
  return true;
}

[[nodiscard]] constexpr bool checked_add(u64 a, u64 b, u64& out) noexcept {
  if (a > kU64Max - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_sub(u64 a, u64 b, u64& out) noexcept {
  if (b > a) {
    return false;
  }
  out = a - b;
  return true;
}

[[nodiscard]] constexpr bool checked_mul(u64 a, u64 b, u64& out) noexcept {
  if (a != 0 && b > kU64Max / a) {
    return false;
  }
  out = a * b;
  return true;
}

[[nodiscard]] constexpr u64 saturating_add(u64 a, u64 b) noexcept {
  return (a > kU64Max - b) ? kU64Max : a + b;
}

[[nodiscard]] constexpr u64 saturating_mul(u64 a, u64 b) noexcept {
  u64 out = 0;
  return checked_mul(a, b, out) ? out : kU64Max;
}

[[nodiscard]] constexpr u64 saturating_sub(u64 a, u64 b) noexcept {
  return (b > a) ? 0 : a - b;
}

// Saturating accumulation of a rate-over-time product with an explicit unit
// denominator, used for token refill so that huge tick gaps cannot wrap.
[[nodiscard]] constexpr u64 saturating_mul_div(u64 a, u64 b, u64 d) noexcept {
  const DivModWide dm = divmod_wide(mul_wide(a, b), d);
  return dm.ok ? dm.quotient : kU64Max;
}

[[nodiscard]] constexpr u64 gcd(u64 a, u64 b) noexcept {
  while (b != 0) {
    const u64 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

[[nodiscard]] constexpr u64 clamp_u64(u64 value, u64 low, u64 high) noexcept {
  if (value < low) {
    return low;
  }
  return value > high ? high : value;
}

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_CHECKED_MATH_HPP
