#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace rgtest {

using namespace rate_governor;  // NOLINT(google-build-using-namespace)

namespace {

std::string read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (valid() && !exited_) {
    kill();
  }
  close_handles();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_),
      process_id_(other.process_id_),
      exit_code_(other.exit_code_),
      exited_(other.exited_),
      stdout_path_(std::move(other.stdout_path_)),
      stderr_path_(std::move(other.stderr_path_)) {
  other.handle_ = 0;
  other.process_id_ = 0;
  other.exited_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (valid() && !exited_) {
      kill();
    }
    close_handles();
    handle_ = other.handle_;
    process_id_ = other.process_id_;
    exit_code_ = other.exit_code_;
    exited_ = other.exited_;
    stdout_path_ = std::move(other.stdout_path_);
    stderr_path_ = std::move(other.stderr_path_);
    other.handle_ = 0;
    other.process_id_ = 0;
    other.exited_ = true;
  }
  return *this;
}

void ChildProcess::close_handles() {
#if defined(_WIN32)
  if (handle_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = 0;
  }
#endif
}

#if defined(_WIN32)
ChildProcess ChildProcess::start(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& stdout_path, const std::string& stderr_path,
                                 std::string& error) {
  ChildProcess child;
  child.stdout_path_ = stdout_path;
  child.stderr_path_ = stderr_path;

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE out_handle = ::CreateFileA(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_handle = ::CreateFileA(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE) {
    error = "cannot create child output files";
    if (out_handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(out_handle);
    }
    if (err_handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(err_handle);
    }
    return child;
  }

  std::string command_line = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command_line.append(" \"");
    command_line.append(argument);
    command_line.append("\"");
  }
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};

  const BOOL started = ::CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
  ::CloseHandle(out_handle);
  ::CloseHandle(err_handle);
  if (started == FALSE) {
    error = "CreateProcess failed with error " + std::to_string(::GetLastError());
    return child;
  }
  ::CloseHandle(process.hThread);
  child.handle_ = reinterpret_cast<std::uintptr_t>(process.hProcess);
  child.process_id_ = static_cast<rate_governor::u64>(process.dwProcessId);
  return child;
}

bool ChildProcess::running() {
  if (!valid() || exited_) {
    return false;
  }
  const DWORD status = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 0);
  return status == WAIT_TIMEOUT;
}

int ChildProcess::wait() {
  if (!valid()) {
    return exit_code_;
  }
  if (!exited_) {
    ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    if (::GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code) != FALSE) {
      exit_code_ = static_cast<int>(code);
    }
    exited_ = true;
  }
  return exit_code_;
}

void ChildProcess::kill() {
  if (!valid()) {
    return;
  }
  ::TerminateProcess(reinterpret_cast<HANDLE>(handle_), 137);
  ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  exited_ = true;
}

#else

ChildProcess ChildProcess::start(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& stdout_path, const std::string& stderr_path,
                                 std::string& error) {
  ChildProcess child;
  child.stdout_path_ = stdout_path;
  child.stderr_path_ = stderr_path;
  std::vector<std::string> storage;
  storage.push_back(executable);
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  for (std::string& item : storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    error = "fork failed";
    return child;
  }
  if (pid == 0) {
    ::freopen(stdout_path.c_str(), "w", stdout);
    ::freopen(stderr_path.c_str(), "w", stderr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  child.handle_ = static_cast<std::uintptr_t>(pid);
  child.process_id_ = static_cast<rate_governor::u64>(pid);
  return child;
}

bool ChildProcess::running() {
  if (!valid() || exited_) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(handle_), &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
}

int ChildProcess::wait() {
  if (!valid()) {
    return exit_code_;
  }
  if (!exited_) {
    int status = 0;
    ::waitpid(static_cast<pid_t>(handle_), &status, 0);
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  return exit_code_;
}

void ChildProcess::kill() {
  if (!valid()) {
    return;
  }
  ::kill(static_cast<pid_t>(handle_), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(handle_), &status, 0);
  exited_ = true;
}

#endif

std::string ChildProcess::read_stdout() const { return read_file(stdout_path_); }
std::string ChildProcess::read_stderr() const { return read_file(stderr_path_); }

ProcessRun run_process(const std::string& executable, const std::vector<std::string>& arguments,
                       std::string_view tag) {
  ProcessRun run;
  ScratchDir scratch(std::string("proc_") + std::string(tag));
  const std::string out_path = scratch.file("stdout.txt");
  const std::string err_path = scratch.file("stderr.txt");
  std::string error;
  ChildProcess child = ChildProcess::start(executable, arguments, out_path, err_path, error);
  if (!child.valid()) {
    run.err = error;
    return run;
  }
  run.started = true;
  run.exit_code = child.wait();
  run.out = child.read_stdout();
  run.err = child.read_stderr();
  return run;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
namespace {

rate_governor::Provenance test_provenance(std::string_view detail) {
  rate_governor::Provenance provenance;
  provenance.source = rate_governor::AuthoritySource::Configuration;
  provenance.detail.assign(detail.substr(0, rate_governor::kMaxProvenanceDetail));
  return provenance;
}

}  // namespace

FixtureIds install_fixture(rate_governor::RateGovernor& engine, const FixtureOptions& options) {
  using namespace rate_governor;
  FixtureIds ids;

  Flow flow;
  flow.id = ids.flow;
  flow.generation = Generation(1);
  flow.state = FlowState::Active;
  flow.provenance = test_provenance("fixture flow");
  (void)engine.put_flow(flow);

  Resource resource;
  resource.id = ids.resource;
  resource.generation = Generation(1);
  resource.capacity_ups = options.resource_capacity_ups;
  resource.capacity_burst_tokens = options.policy_burst_tokens * 4;
  resource.state = ResourceState::Active;
  resource.provenance = test_provenance("fixture resource");
  (void)engine.put_resource(resource);

  Grant grant;
  grant.id = ids.grant;
  grant.flow = ids.flow;
  grant.resource = ids.resource;
  grant.generation = Generation(1);
  grant.ceiling_ups = options.grant_ceiling_ups;
  grant.burst_tokens = options.grant_burst_tokens;
  grant.state = GrantState::Active;
  grant.provenance = test_provenance("fixture grant");
  (void)engine.put_grant(grant);

  if (options.with_reservation) {
    Reservation reservation;
    reservation.id = ids.reservation;
    reservation.flow = ids.flow;
    reservation.resource = ids.resource;
    reservation.generation = Generation(1);
    reservation.ceiling_ups = options.reservation_ceiling_ups;
    reservation.burst_tokens = options.grant_burst_tokens;
    reservation.state = ReservationState::Active;
    reservation.provenance = test_provenance("fixture reservation");
    (void)engine.put_reservation(reservation);
  }

  Policy policy;
  policy.id = ids.policy;
  policy.generation = Generation(1);
  policy.floor_ups = options.policy_floor_ups;
  policy.target_ups = options.policy_target_ups;
  policy.ceiling_ups = options.policy_ceiling_ups;
  policy.burst_tokens = options.policy_burst_tokens;
  policy.refill = RefillSemantics::ContinuousTokenBucket;
  policy.state = PolicyState::Active;
  policy.provenance = test_provenance("fixture policy");
  (void)engine.put_policy(policy);

  if (options.with_reservation) {
    ids.reservation = ReservationId(1);
  }
  return ids;
}

EnvelopeRequest make_request(const FixtureIds& ids, const FixtureOptions& options) {
  EnvelopeRequest request;
  request.flow = ids.flow;
  request.flow_generation = rate_governor::Generation(1);
  request.resource = ids.resource;
  request.resource_generation = rate_governor::Generation(1);
  request.grant = ids.grant;
  request.grant_generation = rate_governor::Generation(1);
  request.policy = ids.policy;
  request.policy_generation = rate_governor::Generation(1);
  request.backend = ids.backend;
  request.backend_generation = rate_governor::Generation(1);
  request.has_reservation = options.with_reservation;
  request.reservation = ids.reservation;
  request.reservation_generation = rate_governor::Generation(1);
  return request;
}

ActorContext fixture_actor(const FixtureOptions& options) {
  return rate_governor::operator_context(options.epoch, options.boot, "test fixture actor");
}

BackendDescriptor fixture_backend_descriptor(BackendId id, Generation generation) {
  BackendDescriptor descriptor;
  descriptor.id = id;
  descriptor.generation = generation;
  descriptor.kind = BackendKind::Synthetic;
  descriptor.capabilities =
      kBackendCapApply | kBackendCapRevoke | kBackendCapVerify | kBackendCapBurst;
  descriptor.verification = VerificationMode::PostApplyReadback;
  descriptor.label = "test synthetic device";
  descriptor.provenance.source = AuthoritySource::Configuration;
  descriptor.provenance.detail = "test fixture backend";
  return descriptor;
}

}  // namespace rgtest
