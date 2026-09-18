// Downstream consumer of the installed Rate Governor package.
//
// It uses only the public API: it installs synthetic authority, opens a
// governed envelope, applies it through the shipped synthetic enforcement
// backend, verifies the effect by readback, and prints what is legally
// enforceable. Nothing here is a physical-network claim.

#include <cstdio>
#include <string>

#include <rate_governor/rate_governor.hpp>

namespace {

rate_governor::Provenance consumer_provenance(std::string_view detail) {
  rate_governor::Provenance provenance;
  provenance.source = rate_governor::AuthoritySource::Configuration;
  provenance.detail.assign(detail);
  return provenance;
}

}  // namespace

int main() {
  using namespace rate_governor;

  ManualClock clock(0);
  EngineConfig config;
  config.durability = DurabilityMode::None;
  config.boot = WorkerBootId(1);

  SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{}, config.boot,
                           config.initial_epoch, "consumer synthetic device");
  RateGovernor engine(config, clock, &backend, nullptr, nullptr);

  Flow flow;
  flow.id = FlowId(1);
  flow.generation = Generation(1);
  flow.state = FlowState::Active;
  flow.provenance = consumer_provenance("consumer flow");
  if (!engine.put_flow(flow)) {
    std::printf("consumer: put_flow failed\n");
    return 1;
  }

  Resource resource;
  resource.id = ResourceId(1);
  resource.generation = Generation(1);
  resource.capacity_ups = 10000;
  resource.state = ResourceState::Active;
  resource.provenance = consumer_provenance("consumer resource");
  (void)engine.put_resource(resource);

  Grant grant;
  grant.id = GrantId(1);
  grant.flow = FlowId(1);
  grant.resource = ResourceId(1);
  grant.generation = Generation(1);
  grant.ceiling_ups = 4000;
  grant.burst_tokens = 8000;
  grant.state = GrantState::Active;
  grant.provenance = consumer_provenance("consumer grant");
  (void)engine.put_grant(grant);

  Policy policy;
  policy.id = PolicyId(1);
  policy.generation = Generation(1);
  policy.floor_ups = 500;
  policy.target_ups = 2000;
  policy.ceiling_ups = 4000;
  policy.burst_tokens = 8000;
  policy.refill = RefillSemantics::ContinuousTokenBucket;
  policy.state = PolicyState::Active;
  policy.provenance = consumer_provenance("consumer policy");
  (void)engine.put_policy(policy);

  BackendDescriptor descriptor = backend.describe();
  if (!engine.put_backend(descriptor)) {
    std::printf("consumer: put_backend failed\n");
    return 1;
  }

  const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "consumer actor");

  EnvelopeRequest request;
  request.flow = FlowId(1);
  request.resource = ResourceId(1);
  request.grant = GrantId(1);
  request.policy = PolicyId(1);
  request.backend = BackendId(1);
  const Result<RateEnvelopeId> opened = engine.open_envelope(request, actor);
  if (!opened) {
    std::printf("consumer: open_envelope failed: %s\n", opened.message().c_str());
    return 1;
  }

  const Result<ApplyDispatch> applied = engine.apply(opened.value(), actor);
  if (!applied || !applied.value().verified) {
    std::printf("consumer: apply was not verified\n");
    return 1;
  }

  const Result<EnvelopeView> view = engine.inspect(opened.value());
  if (!view) {
    std::printf("consumer: inspect failed\n");
    return 1;
  }
  std::printf("consumer: %s\n", describe(view.value()).c_str());
  std::printf("consumer: Rate Governor %s usable from an installed package\n",
              std::string(kVersionString).c_str());
  if (!view.value().legally_enforceable_now) {
    std::printf("consumer: envelope is not enforceable\n");
    return 1;
  }

  Result<u64> burst = engine.consume_burst(opened.value(), 100);
  if (!burst) {
    std::printf("consumer: burst consumption refused: %s\n", burst.message().c_str());
    return 1;
  }
  std::printf("consumer: burst remaining after spending 100 = %llu\n",
              static_cast<unsigned long long>(burst.value()));
  return 0;
}
