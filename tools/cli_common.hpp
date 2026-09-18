#ifndef RATE_GOVERNOR_TOOLS_CLI_COMMON_HPP
#define RATE_GOVERNOR_TOOLS_CLI_COMMON_HPP

// Small shared helpers for the command line tools. Header only: the tools are
// thin adapters over the library and share nothing but argument handling.

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rate_governor/rate_governor.hpp"

namespace rate_governor::tools {

class Arguments {
 public:
  Arguments(int argc, char** argv, int first) {
    for (int index = first; index < argc; ++index) {
      std::string token = argv[index];
      if (token.rfind("--", 0) == 0) {
        std::string value;
        if (index + 1 < argc && std::string_view(argv[index + 1]).rfind("--", 0) != 0) {
          value = argv[++index];
        }
        flags_[token] = value;
      } else {
        positional_.push_back(std::move(token));
      }
    }
  }

  [[nodiscard]] bool has(std::string_view flag) const {
    return flags_.find(std::string(flag)) != flags_.end();
  }

  [[nodiscard]] std::string get(std::string_view flag, std::string fallback = {}) const {
    const auto found = flags_.find(std::string(flag));
    if (found == flags_.end() || found->second.empty()) {
      return fallback;
    }
    return found->second;
  }

  [[nodiscard]] u64 get_u64(std::string_view flag, u64 fallback) const {
    const std::string text = get(flag);
    u64 value = 0;
    if (text.empty() || !parse_u64_strict(text, value)) {
      return fallback;
    }
    return value;
  }

  [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string, std::less<>> flags_;
  std::vector<std::string> positional_;
};

inline void print_line(const std::string& text) { std::fputs(text.c_str(), stdout); std::fputc('\n', stdout); }

inline const char* yes_no(bool value) { return value ? "yes" : "no"; }

}  // namespace rate_governor::tools

#endif  // RATE_GOVERNOR_TOOLS_CLI_COMMON_HPP
