#ifndef RATE_GOVERNOR_PROVENANCE_HPP
#define RATE_GOVERNOR_PROVENANCE_HPP

// Provenance records who asserted a fact and under which incarnation.
//
// Missing or stale evidence never becomes positive authority: a Provenance
// whose source is Unknown is never treated as an authoritative assertion, and a
// Provenance carrying a superseded epoch or boot id is rejected by every
// consumer that checks it.

#include <cstdint>
#include <string>
#include <string_view>

#include "rate_governor/ids.hpp"

namespace rate_governor {

enum class AuthoritySource : std::uint8_t {
  Unknown = 0,
  Operator,
  Configuration,
  Recovery,
  BackendReport,
  Derived,
};

[[nodiscard]] std::string_view to_string_view(AuthoritySource source) noexcept;

inline constexpr std::size_t kMaxProvenanceDetail = 96;

[[nodiscard]] bool is_bounded_text(std::string_view text, std::size_t max_length) noexcept;

struct Provenance {
  AuthoritySource source{AuthoritySource::Unknown};
  FabricEpoch epoch{};
  WorkerBootId boot{};
  u64 sequence{0};
  std::string detail;

  [[nodiscard]] bool authoritative() const noexcept {
    return source != AuthoritySource::Unknown;
  }

  [[nodiscard]] bool well_formed() const noexcept {
    return is_bounded_text(detail, kMaxProvenanceDetail);
  }
};

[[nodiscard]] std::string describe(const Provenance& provenance);

// The incarnation context a caller presents when it asks the engine to mutate
// authoritative state. Both fields are fenced: a request bearing a stale fabric
// epoch or an unknown coordinator boot id is rejected before it can mutate.
struct ActorContext {
  FabricEpoch epoch{};
  WorkerBootId boot{};
  Provenance provenance;

  [[nodiscard]] bool has_incarnation() const noexcept {
    return epoch.valid() && boot.valid();
  }
};

[[nodiscard]] ActorContext operator_context(FabricEpoch epoch, WorkerBootId boot,
                                            std::string_view detail);

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_PROVENANCE_HPP
