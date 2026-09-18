#ifndef RATE_GOVERNOR_TEST_SUPPORT_HPP
#define RATE_GOVERNOR_TEST_SUPPORT_HPP

// Minimal, dependency-free test support.
//
// Deliberately plain: a test either completes or it does not. There is no
// watchdog, no per-test timeout and no retry. A test that hangs is a defect to
// diagnose, not something to paper over.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rate_governor/rate_governor.hpp"

namespace rgtest {

// Aborts the current test but keeps the suite running.
class TestAbort : public std::exception {
 public:
  explicit TestAbort(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

[[nodiscard]] inline std::string hex64(rate_governor::u64 value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
  return buffer;
}

class TestContext {
 public:
  TestContext(std::string suite, std::string name)
      : suite_(std::move(suite)), name_(std::move(name)) {}

  void check(bool condition, const std::string& expression, const char* file, int line) {
    if (condition) {
      return;
    }
    ++failures_;
    std::ostringstream stream;
    stream << suite_ << "." << name_ << ":" << line << ": check failed: " << expression;
    if (!notes_.empty()) {
      stream << " | " << notes_;
      notes_.clear();
    }
    if (current_phase_.empty()) {
      stream << " [" << file << "]";
    } else {
      stream << " [" << current_phase_ << "]";
    }
    failures_log_.push_back(stream.str());
    std::printf("FAIL %s\n", stream.str().c_str());
  }

  template <class A, class B>
  void check_eq(const A& left, const B& right, const std::string& expression, const char* file,
                int line) {
    if (left == right) {
      return;
    }
    std::ostringstream stream;
    stream << expression << " (left=" << left << " right=" << right << ")";
    check(false, stream.str(), file, line);
  }

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw TestAbort(message);
    }
  }

  void note(std::string text) { notes_ = std::move(text); }

  void phase(std::string text) { current_phase_ = std::move(text); }

  [[nodiscard]] int failures() const { return failures_; }
  [[nodiscard]] const std::vector<std::string>& failures_log() const { return failures_log_; }

 private:
  std::string suite_;
  std::string name_;
  std::string notes_;
  std::string current_phase_;
  int failures_{0};
  std::vector<std::string> failures_log_;
};

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string suite;
  std::string name;
  TestFunction function{nullptr};
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

class Registrar {
 public:
  Registrar(const char* suite, const char* name, TestFunction function) {
    registry().push_back(TestCase{suite, name, function});
  }
};

// Deterministic generator: never seeded from the clock, never from the address
// space, so a failing sequence is exactly reproducible from its seed alone.
class Rng {
 public:
  explicit Rng(rate_governor::u64 seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] rate_governor::u64 next() { return rate_governor::splitmix64(state_); }

  [[nodiscard]] rate_governor::u64 below(rate_governor::u64 bound) {
    return bound == 0 ? 0 : next() % bound;
  }

  [[nodiscard]] bool chance(rate_governor::u64 numerator, rate_governor::u64 denominator) {
    return below(denominator) < numerator;
  }

 private:
  rate_governor::u64 state_;
};

// Scratch directory per test, removed by the caller.
class ScratchDir {
 public:
  explicit ScratchDir(std::string_view tag) {
    const auto base = std::filesystem::temp_directory_path();
    path_ = base / ("rgtest_" + std::string(tag) + "_" + std::to_string(counter()));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }

  ~ScratchDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  [[nodiscard]] std::string file(std::string_view name) const {
    return (path_ / std::string(name)).string();
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  static rate_governor::u64 counter() {
    static rate_governor::u64 value = 0;
    return ++value;
  }

  std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Child processes. Real OS processes: a multiprocess claim that did not cross a
// process boundary is not a multiprocess claim.
// ---------------------------------------------------------------------------
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  // Starts executable with argument list (argv[0] style excluded). stdout and
  // stderr are redirected to files so a failure can be inspected afterwards.
  [[nodiscard]] static ChildProcess start(const std::string& executable,
                                          const std::vector<std::string>& arguments,
                                          const std::string& stdout_path,
                                          const std::string& stderr_path, std::string& error);

  [[nodiscard]] bool valid() const noexcept { return handle_ != 0; }
  [[nodiscard]] rate_governor::u64 process_id() const noexcept { return process_id_; }
  [[nodiscard]] bool running();
  // Blocks until the process ends. No timeout: a hang is a defect.
  [[nodiscard]] int wait();
  void kill();
  [[nodiscard]] std::string read_stdout() const;
  [[nodiscard]] std::string read_stderr() const;

 private:
  void close_handles();

  std::uintptr_t handle_{0};
  rate_governor::u64 process_id_{0};
  int exit_code_{-1};
  bool exited_{false};
  std::string stdout_path_;
  std::string stderr_path_;
};

// A single run of a child process with its exit code and captured output.
struct ProcessRun {
  int exit_code{-1};
  std::string out;
  std::string err;
  bool started{false};
};

[[nodiscard]] ProcessRun run_process(const std::string& executable,
                                     const std::vector<std::string>& arguments,
                                     std::string_view tag);

// ---------------------------------------------------------------------------
// Shared engine fixtures
// ---------------------------------------------------------------------------
struct FixtureIds {
  rate_governor::FlowId flow{rate_governor::FlowId(1)};
  rate_governor::ResourceId resource{rate_governor::ResourceId(1)};
  rate_governor::GrantId grant{rate_governor::GrantId(1)};
  rate_governor::ReservationId reservation{rate_governor::ReservationId(1)};
  rate_governor::PolicyId policy{rate_governor::PolicyId(1)};
  rate_governor::BackendId backend{rate_governor::BackendId(1)};
};

struct FixtureOptions {
  rate_governor::u64 grant_ceiling_ups{1000};
  rate_governor::u64 reservation_ceiling_ups{900};
  rate_governor::u64 resource_capacity_ups{4000};
  rate_governor::u64 policy_floor_ups{100};
  rate_governor::u64 policy_target_ups{600};
  rate_governor::u64 policy_ceiling_ups{1000};
  rate_governor::u64 policy_burst_tokens{2000};
  rate_governor::u64 grant_burst_tokens{2000};
  bool with_reservation{false};
  rate_governor::FabricEpoch epoch{rate_governor::FabricEpoch(3)};
  rate_governor::WorkerBootId boot{rate_governor::WorkerBootId(9)};
};

// Installs flow, resource, grant, policy (and optionally a reservation) with
// generation 1 and Active state.
[[nodiscard]] FixtureIds install_fixture(rate_governor::RateGovernor& engine,
                                         const FixtureOptions& options);

[[nodiscard]] rate_governor::EnvelopeRequest make_request(const FixtureIds& ids,
                                                          const FixtureOptions& options);

[[nodiscard]] rate_governor::ActorContext fixture_actor(const FixtureOptions& options);

[[nodiscard]] rate_governor::BackendDescriptor fixture_backend_descriptor(
    rate_governor::BackendId id, rate_governor::Generation generation);

}  // namespace rgtest

#define RG_TEST(suite_name, test_name)                                            \
  static void suite_name##_##test_name(::rgtest::TestContext&);                   \
  static ::rgtest::Registrar rg_registrar_##suite_name##_##test_name(             \
      #suite_name, #test_name, &suite_name##_##test_name);                        \
  static void suite_name##_##test_name(::rgtest::TestContext& ctx)

#define RG_CHECK(expression) ctx.check((expression), #expression, __FILE__, __LINE__)
#define RG_CHECK_EQ(left, right)   ctx.check_eq((left), (right), #left " == " #right, __FILE__, __LINE__)
#define RG_REQUIRE(expression) ctx.require((expression), #expression " at " __FILE__ ":" + std::to_string(__LINE__))
#define RG_NOTE(text) ctx.note(text)
#define RG_PHASE(text) ctx.phase(text)

#endif  // RATE_GOVERNOR_TEST_SUPPORT_HPP
