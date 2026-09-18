# Rate Governor

**Open-source, vendor-neutral C++20 runtime for generation-bound enforcement of
explicit network rate envelopes, ceilings, floors, bursts and revocation across
governed flows and resources.**

Rate Governor answers one question, deterministically and explainably:

> Given an authoritative flow/resource binding, granted or reserved bandwidth, an
> explicit rate policy, a burst allowance, service obligations, current
> generations and a named enforcement backend, what rate envelope is **legally
> enforceable right now**, how much burst remains, what effect has **actually been
> applied**, and when must the envelope be reduced, revoked, revalidated, fenced,
> or rejected as stale?

It owns rate-envelope authority and enforcement-state governance. It does not
arbitrate who gets bandwidth, create reservations, admit traffic, choose paths,
schedule packets, define priority or QoS semantics, compute congestion control, or
program vendor devices except through a narrow verified backend abstraction.

## Status of this release: what is real and what is not

Read this before using anything here.

| Aspect | Status |
| --- | --- |
| Rate/burst arithmetic, plan derivation, lifecycle, attempts, revocation, staleness, fencing, revalidation | **REAL** - implemented, deterministic, tested. |
| Durable journal, crash-safe commit protocol, recovery classification | **REAL** - implemented and tested, including torn tails and corruption. |
| Multi-process control plane over real loopback sockets with a real worker process, real epochs and real boot-id fencing | **REAL** - implemented and tested with real OS processes and framed transport. |
| Enforcement of an actual rate on an actual interface | **NOT IMPLEMENTED**. The only enforcement backend in this release is a software table with a readback, labelled SYNTHETIC everywhere it appears. |
| Physical shaping, packet pacing, NIC/DPU/switch offload, RDMA, optics | **NOT CLAIMED** and not tested. There is no hardware validation in this repository. |
| Benchmarks | **SYNTHETIC** control-plane bookkeeping only (see `docs/BENCHMARKS.md`). No packet-rate or throughput claim is made anywhere. |

If you need a rate actually enforced on a wire, implement
`rate_governor::IEnforcementBackend` against your device. Nothing else changes:
the authority, lifecycle, durability and fencing semantics stay as they are.

## Build and test

Requirements: CMake 3.20+, a C++20 compiler. On Windows, MSVC 19.3x with the
Windows SDK; on other platforms a conforming GCC/Clang.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure      # or run build/tests/rg_tests

First-party targets are built with strict warnings and warnings-as-errors
(`/W4 /WX /permissive-` on MSVC, `-Wall -Wextra -Wpedantic -Werror` elsewhere).
Optional build flags:

| Flag | Effect |
| --- | --- |
| `RATE_GOVERNOR_BUILD_TESTS` | Build the test suite (default ON). |
| `RATE_GOVERNOR_BUILD_TOOLS` | Build `rgctl`, `rg_backend_worker`, `rg_bench` (default ON). |
| `RATE_GOVERNOR_ENABLE_ASAN` | Build with AddressSanitizer (default OFF). |
| `RATE_GOVERNOR_ENABLE_ANALYZE` | Run the MSVC static analyzer over first-party code (default OFF). |

## Install and consume

    cmake --install build --prefix /some/prefix

An independent consumer then does:

    find_package(RateGovernor 1.0 CONFIG REQUIRED)
    target_link_libraries(app PRIVATE RateGovernor::rate_governor)

`examples/consumer` is exactly that: a separate CMake project, not part of this
build, that is configured against an installed prefix and prints the envelope it
made enforceable. See `docs/VALIDATION.md` for the recorded run.

## Quick start

    #include <rate_governor/rate_governor.hpp>
    using namespace rate_governor;

    ManualClock clock(0);
    EngineConfig config;                       // durability defaults to Strict
    config.boot = WorkerBootId(1);
    SyntheticBackend backend(BackendId(1), Generation(1), SyntheticDeviceConfig{},
                             config.boot, config.initial_epoch, "synthetic device");
    RateGovernor engine(config, clock, &backend, nullptr, nullptr);

    // Install authority: flow, resource, grant, policy (and a reservation if you
    // have one). Each carries the generation that makes it current.
    // ...

    const ActorContext actor = operator_context(engine.epoch(), engine.boot(), "operator");
    EnvelopeRequest request;
    request.flow = FlowId(1);
    request.resource = ResourceId(1);
    request.grant = GrantId(1);
    request.policy = PolicyId(1);
    request.backend = BackendId(1);

    const RateEnvelopeId envelope = engine.open_envelope(request, actor).value();
    const ApplyDispatch applied = engine.apply(envelope, actor).value();
    const EnvelopeView view = engine.inspect(envelope).value();

`view` is the answer to the core question: `legally_enforceable_now`,
`effective_ceiling_ups` and friends (what is authorized), `burst_tokens_available`
(how much burst remains), `applied_rate_ups` with `effect_verified` (what was
actually applied and read back), and `action` with `action_reason` (what must
happen next: reduce, revoke, revalidate, fence, reject as stale). `describe(view)`
renders the same facts as one line.

## Model in one page

* **Strongly typed identities.** `RateEnvelopeId`, `FlowId`, `ResourceId`,
  `GrantId`, `ReservationId`, `PolicyId`, `BackendId`, `EnforcementAttemptId`,
  `Generation`, `FabricEpoch`, `WorkerBootId`. A generation can never be passed
  where a flow id is expected.
* **Exact integer arithmetic.** Rates, bursts and durations are `u64`. `mul_div`
  and friends report overflow instead of wrapping. No floating point participates
  in a decision.
* **Explicit time.** Every decision receives its instant from an `IClock`; a
  `ManualClock` makes any scenario reproducible.
* **Fingerprints.** Every plan records every generation *and* every raw magnitude
  it used, so a decision can be re-derived and compared field by field.
* **Provenance.** Every assertion says who made it. `AuthoritySource::Unknown` is
  never positive authority.

Envelope lifecycle: `desired -> authorized -> dispatching -> applied`, with
`degraded`, `revoke-pending`, `revoked`, `stale` and `failed` for everything else.
Attempt lifecycle: `created -> dispatched -> acknowledged -> verified`, with
`failed`, `cancelled`, `abandoned`, `ambiguous`, `late-rejected` and
`duplicate-ignored`.

## Invariants you can rely on

1. The effective ceiling never exceeds the funding authority that supports it.
2. `floor <= target <= ceiling` always holds, or the envelope is denied with a
   reason instead of being quietly adjusted.
3. Burst accounting never overflows and never goes negative.
4. Stale grant, reservation, policy, resource, flow or backend invalidates the
   envelope; stale funding never continues to authorize an old envelope.
5. A cancelled, abandoned, ambiguous or late attempt can never publish success.
6. Duplicate apply, duplicate revoke and duplicate completions are idempotent.
7. `applied` is never inferred from an acknowledgement: a post-apply readback must
   confirm the exact requested envelope. There is no switch to skip it.

## Durability

Mutations follow *validate -> bind authority -> plan -> reserve -> journal ->
perform work -> verify -> commit -> retire*. A commit flushes and calls the
platform durability barrier before it reports success, so nothing is acknowledged
before its state is durable.

Recovery restores durable configuration and committed authoritative state as
**history**, never as current fact: unfinished attempts become `ambiguous`, every
effect claim is demoted to "requires revalidation", and process liveness,
sessions, leases and publisher authority are never restored. Recovery always
advances the fabric epoch, which fences everything the previous incarnation could
have left in force.

## Tools

| Tool | Purpose |
| --- | --- |
| `rgctl` | `version`, `selfcheck`, `demo`, `inspect`, `apply`, `recover`, `revalidate`, `revoke`, `journal-scan`. Operates either offline or against a live worker, and starts and stops real coordinator incarnations against a durable journal. |
| `rg_backend_worker` | The SYNTHETIC enforcement worker process. `--listen` serves concurrent sessions from one shared device; `--connect` dials a coordinator that is already listening. |
| `rg_bench` | SYNTHETIC control-plane bookkeeping benchmark. Labels itself as such. |

A typical multi-process walkthrough:

    rg_backend_worker --listen 127.0.0.1:45871 &
    rgctl apply      --journal state.rgjournal --endpoint 127.0.0.1:45871
    rgctl recover    --journal state.rgjournal --endpoint 127.0.0.1:45871
    rgctl revalidate --journal state.rgjournal --endpoint 127.0.0.1:45871
    rgctl inspect    --journal state.rgjournal

## Validation

See `docs/VALIDATION.md` for the recorded evidence: Debug/Release/ASan test runs,
the multi-process proofs, the persistence and restart proofs, the install and
downstream `find_package` consumer, the fresh-clone build, and the honest list of
what was not validated.

## Documentation map

| Document | Contents |
| --- | --- |
| `docs/ARCHITECTURE.md` | Model, boundary, plan derivation, lifecycle, races, locking discipline, durability, transport. |
| `docs/VALIDATION.md` | What was built, what was proven, how, and what was not. |
| `docs/BENCHMARKS.md` | Synthetic control-plane measurements. |

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
