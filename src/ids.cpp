#include "rate_governor/ids.hpp"

#include <string>

namespace rate_governor {

bool parse_u64_strict(std::string_view text, u64& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  u64 value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const u64 digit = static_cast<u64>(ch - '0');
    if (!checked_mul(value, 10, value)) {
      return false;
    }
    if (!checked_add(value, digit, value)) {
      return false;
    }
  }
  out = value;
  return true;
}

std::string join_identity(std::string_view prefix, u64 value) {
  std::string out;
  out.reserve(prefix.size() + 1 + 20);
  out.append(prefix);
  out.push_back(':');
  out.append(std::to_string(value));
  return out;
}

}  // namespace rate_governor
