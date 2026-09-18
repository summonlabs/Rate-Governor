// rg_bench - SYNTHETIC control-plane bookkeeping benchmark.
//
// What is measured: completed control-plane operations per second, and the
// exact integer arithmetic and durability work each of them performs. What is
// NOT measured and NOT claimed: packet rates, link throughput, shaping accuracy,
// scheduling latency, or any property of a real network device. Every number
// this program prints is a synthetic software figure for one machine.

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli_common.hpp"
#include "rate_governor/rate_governor.hpp"
#include "scenario.hpp"

namespace {

using namespace rate_governor;
using rate_governor::tools::Arguments;

struct Measurement {
  std::string name;
  std::string unit;
  u64 iterations{0};
  double seconds{0.0};
  double value() const { return seconds > 0.0 ? static_cast<double>(iterations) / seconds : 0.0; }
};

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  [[nodiscard]] double seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

struct BenchConfig {
  u64 iterations{2000};
  u64 ceiling_ups{1000000};
  u64 burst_tokens{100000};
  bool json{false};
  std::string journal_path;
};

Measurement bench_plan(const BenchConfig& config) {
  Measurement measurement;
  measurement.name = "plan_derivation";
  measurement.unit = "plans/s";
  ManualClock clock(0);
  EngineConfig engine_config;
  engine_config.durability = DurabilityMode::None;
  engine_config.max_envelopes = config.iterations + 8;
  SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, WorkerBootId(1),
                           engine_config.initial_epoch, "bench");
  RateGovernor engine(engine_config, clock, &backend, nullptr, nullptr);
  rate_governor::tools::ScenarioOptions options;
  options.epoch = engine.epoch();
  options.boot = engine.boot();
  options.ceiling_ups = config.ceiling_ups;
  options.burst_tokens = config.burst_tokens;
  const rate_governor::tools::ScenarioIds ids = rate_governor::tools::install_scenario(engine, options);
  const ActorContext actor = rate_governor::tools::scenario_actor(options);
  BackendDescriptor descriptor = backend.describe();
  descriptor.provenance = rate_governor::tools::scenario_provenance("bench backend");
  (void)engine.put_backend(descriptor);

  EnvelopeRequest request;
  request.flow = ids.flow;
  request.resource = ids.resource;
  request.grant = ids.grant;
  request.policy = ids.policy;
  request.backend = ids.backend;
  request.has_reservation = true;
  request.reservation = ids.reservation;
  const Result<RateEnvelopeId> opened = engine.open_envelope(request, actor);
  if (!opened) {
    std::printf("bench: cannot open the benchmark envelope: %s\n", opened.message().c_str());
    return measurement;
  }
  const RateEnvelopeId envelope = opened.value();

  measurement.iterations = config.iterations;
  Timer timer;
  u64 authorized = 0;
  for (u64 index = 0; index < config.iterations; ++index) {
    const Result<PlanOutcome> plan = engine.explain_plan(envelope);
    if (plan && plan.value().authorized) {
      ++authorized;
    }
  }
  measurement.seconds = timer.seconds();
  if (authorized != config.iterations) {
    std::printf("bench: warning: %llu of %llu plans were denied\n",
                static_cast<unsigned long long>(authorized),
                static_cast<unsigned long long>(config.iterations));
  }
  return measurement;
}

Measurement bench_lifecycle(const BenchConfig& config) {
  Measurement measurement;
  measurement.name = "authorize_apply_verify_revoke";
  measurement.unit = "cycles/s";
  ManualClock clock(0);
  EngineConfig engine_config;
  engine_config.durability = DurabilityMode::None;
  engine_config.max_envelopes = config.iterations + 8;
  SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, WorkerBootId(1),
                           engine_config.initial_epoch, "bench");
  RateGovernor engine(engine_config, clock, &backend, nullptr, nullptr);
  rate_governor::tools::ScenarioOptions options;
  options.epoch = engine.epoch();
  options.boot = engine.boot();
  options.ceiling_ups = config.ceiling_ups;
  options.burst_tokens = config.burst_tokens;
  const rate_governor::tools::ScenarioIds ids = rate_governor::tools::install_scenario(engine, options);
  const ActorContext actor = rate_governor::tools::scenario_actor(options);
  BackendDescriptor descriptor = backend.describe();
  descriptor.provenance = rate_governor::tools::scenario_provenance("bench backend");
  (void)engine.put_backend(descriptor);

  measurement.iterations = config.iterations;
  u64 completed = 0;
  Timer timer;
  for (u64 index = 0; index < config.iterations; ++index) {
    EnvelopeRequest request;
    request.flow = ids.flow;
    request.resource = ids.resource;
    request.grant = ids.grant;
    request.policy = ids.policy;
    request.backend = ids.backend;
    request.has_reservation = true;
    request.reservation = ids.reservation;
    const Result<RateEnvelopeId> envelope = engine.open_envelope(request, actor);
    if (!envelope) {
      continue;
    }
    const Result<ApplyDispatch> applied = engine.apply(envelope.value(), actor);
    if (!applied || !applied.value().verified) {
      continue;
    }
    const Result<RevokeDispatch> revoked =
        engine.revoke(envelope.value(), ReasonCode::EnvelopeRevokedByOperator, actor);
    if (revoked) {
      ++completed;
    }
    clock.advance(1000);
  }
  measurement.seconds = timer.seconds();
  measurement.iterations = completed;
  return measurement;
}

Measurement bench_durable(const BenchConfig& config) {
  Measurement measurement;
  measurement.name = "durable_apply_verify_revoke";
  measurement.unit = "cycles/s";
  ManualClock clock(0);
  JournalConfig journal_config;
  journal_config.path = config.journal_path;
  journal_config.max_bytes = 32ULL * 1024ULL * 1024ULL;
  JournalResult open_result;
  std::unique_ptr<Journal> journal = Journal::open(journal_config, true, open_result);
  if (!journal) {
    measurement.name.append(" (journal unavailable)");
    return measurement;
  }
  EngineConfig engine_config;
  engine_config.durability = DurabilityMode::Strict;
  engine_config.max_envelopes = config.iterations + 8;
  SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, WorkerBootId(1),
                           engine_config.initial_epoch, "bench");
  RateGovernor engine(engine_config, clock, &backend, journal.get(), nullptr);
  rate_governor::tools::ScenarioOptions options;
  options.epoch = engine.epoch();
  options.boot = engine.boot();
  options.ceiling_ups = config.ceiling_ups;
  options.burst_tokens = config.burst_tokens;
  const rate_governor::tools::ScenarioIds ids = rate_governor::tools::install_scenario(engine, options);
  const ActorContext actor = rate_governor::tools::scenario_actor(options);
  BackendDescriptor descriptor = backend.describe();
  descriptor.provenance = rate_governor::tools::scenario_provenance("bench backend");
  (void)engine.put_backend(descriptor);

  u64 completed = 0;
  Timer timer;
  for (u64 index = 0; index < config.iterations; ++index) {
    EnvelopeRequest request;
    request.flow = ids.flow;
    request.resource = ids.resource;
    request.grant = ids.grant;
    request.policy = ids.policy;
    request.backend = ids.backend;
    request.has_reservation = true;
    request.reservation = ids.reservation;
    const Result<RateEnvelopeId> envelope = engine.open_envelope(request, actor);
    if (!envelope) {
      continue;
    }
    const Result<ApplyDispatch> applied = engine.apply(envelope.value(), actor);
    if (!applied || !applied.value().verified) {
      continue;
    }
    const Result<RevokeDispatch> revoked =
        engine.revoke(envelope.value(), ReasonCode::EnvelopeRevokedByOperator, actor);
    if (revoked && revoked.value().verified) {
      ++completed;
    }
    clock.advance(1000);
  }
  measurement.seconds = timer.seconds();
  measurement.iterations = completed;
  journal->close();
  return measurement;
}

Measurement bench_bucket(const BenchConfig& config) {
  Measurement measurement;
  measurement.name = "token_bucket_refill";
  measurement.unit = "refills/s";
  TokenBucket bucket;
  configure_bucket(bucket, config.burst_tokens, config.ceiling_ups, 0);
  TimestampNs now = 0;
  u64 consumed = 0;
  measurement.iterations = config.iterations;
  Timer timer;
  for (u64 index = 0; index < config.iterations; ++index) {
    now += 1000;
    const RefillOutcome outcome = refill_bucket(bucket, now);
    if (outcome.tokens_available != 0 && try_consume(bucket, 1)) {
      ++consumed;
    }
  }
  measurement.seconds = timer.seconds();
  if (consumed == 0) {
    measurement.name.append(" (nothing consumed)");
  }
  return measurement;
}

void print_measurement(const Measurement& measurement, bool json) {
  if (json) {
    std::printf("{\"name\":\"%s\",\"unit\":\"%s\",\"operations\":%llu,\"seconds\":%.6f,"
                "\"value\":%.2f}\n",
                measurement.name.c_str(), measurement.unit.c_str(),
                static_cast<unsigned long long>(measurement.iterations), measurement.seconds,
                measurement.value());
    return;
  }
  std::printf("%-36s %12llu ops %10.6f s %16.2f %s\n", measurement.name.c_str(),
              static_cast<unsigned long long>(measurement.iterations), measurement.seconds,
              measurement.value(), measurement.unit.c_str());
}

int run_benchmark(int argc, char** argv) {
  Arguments args(argc, argv, 1);
  if (args.has("--help")) {
    std::fputs(
        "rg_bench - SYNTHETIC control-plane bookkeeping benchmark\n"
        "\n"
        "usage: rg_bench [--iterations N] [--json] [--journal PATH] [--ceiling N] [--burst N]\n"
        "\n"
        "Measures completed control-plane operations per second: plan derivation, envelope\n"
        "lifecycle with in-process synthetic enforcement, the same lifecycle with a durable\n"
        "journal and a durability barrier, and exact token-bucket refill. It does NOT measure\n"
        "packet rates, shaping accuracy or any physical network property, and it does not\n"
        "claim to.\n",
        stdout);
    return 0;
  }

  BenchConfig config;
  config.iterations = args.get_u64("--iterations", 2000);
  config.ceiling_ups = args.get_u64("--ceiling", 1000000);
  config.burst_tokens = args.get_u64("--burst", 100000);
  config.json = args.has("--json");
  config.journal_path = args.get("--journal", "rg_bench.rgjournal");
  if (config.iterations == 0) {
    config.iterations = 1;
  }

  if (!config.json) {
    std::printf("Rate Governor %s synthetic control-plane benchmark\n",
                std::string(kVersionString).c_str());
    std::printf("label: SYNTHETIC - completed software control-plane operations only; "
                "no packet-rate or device claim\n");
    std::printf("iterations=%llu ceiling=%llu ups burst=%llu tokens\n",
                static_cast<unsigned long long>(config.iterations),
                static_cast<unsigned long long>(config.ceiling_ups),
                static_cast<unsigned long long>(config.burst_tokens));
    std::printf("%-36s %14s %12s %16s %s\n", "measurement", "work", "elapsed", "rate", "unit");
  }

  const std::vector<Measurement> measurements = {
      bench_bucket(config),
      bench_plan(config),
      bench_lifecycle(config),
      bench_durable(config),
  };
  for (const Measurement& measurement : measurements) {
    print_measurement(measurement, config.json);
  }
  if (!config.json) {
    std::printf(
        "note: the durable lifecycle is bounded by the platform durability barrier per\n"
        "      transaction, not by CPU work. The synthetic device holds a software table and\n"
        "      performs no shaping on any interface.\n");
  }
  std::remove(config.journal_path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run_benchmark(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "rg_bench: unexpected failure: %s\n", error.what());
    return 4;
  }
}
