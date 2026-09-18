# Validation report - Rate Governor 1.0.0

Everything below was executed on this repository. Where a claim is bounded, the
bound is stated rather than implied.

## 1. Build configurations

| Configuration | Compiler settings | Result |
| --- | --- | --- |
| Debug | MSVC 19.44 x64, `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`, C++20 | clean |
| Release | MSVC 19.44 x64, `/O2`, same strict warnings | clean |
| Debug + AddressSanitizer | `/fsanitize=address /Zi` | clean, no findings |
| Release + static analysis | MSVC `/analyze` over the library | clean after one fix (`7) |

GCC and Clang are not installed on the validation host, so no GCC/Clang build was
performed. That is stated rather than glossed over: the sources are portable
(no compiler-specific extensions outside the documented Windows/POSIX guards in
`src/ipc.cpp` and the `_commit`/`fsync` barrier), but portability to those
compilers is unverified here.

## 2. Test suite

    build/tests/rg_tests            # 96 tests, no timeouts, no retries

| Run | Tests | Failed |
| --- | --- | --- |
| Debug | 96 | 0 |
| Release | 96 | 0 |
| AddressSanitizer | 96 | 0 |

Categories covered: exact-integer arithmetic (unit plus property over seeded
triples and exhaustive small ranges), token-bucket exactness and boundaries,
identity/codec round trips, wire framing and adversarial mutation (thousands of
malformed frames), plan derivation (including a randomized authority sweep),
lifecycle and lifecycle failure modes, races and fenced completions, journal
durability (torn tail, corruption, bad magic, version, bounds, compaction),
recovery and revalidation, concurrency with a re-entrant event sink, seeded
randomized operation sequences with per-step invariants, and real multi-process
scenarios.

No test uses a timeout. A test that hangs is treated as a defect: two hangs were
found this way and fixed (`7.8, `7.9).

## 3. REAL / SYNTHETIC / UNSUPPORTED proof surface

| Claim | Kind | Proof |
| --- | --- | --- |
| Exact integer rate/burst/plan arithmetic | REAL | Property tests against an independent 128-bit reference; exhaustive small-range cross-checks; overflow detection. |
| Generation-bound authority (stale funding rejected) | REAL | Tests for every mismatch and window case, plus a randomized authority sweep asserting `ceiling <= funding`. |
| Verified effect semantics (acknowledgement is not effect) | REAL | Tests where the readback contradicts the acknowledgement; the envelope degrades and never publishes applied. |
| Cancellation and revocation safety | REAL | Cancel-then-complete, abandon, deadline expiry, and duplicate completion tests. |
| Durable journal, crash safety, recovery classification | REAL | Journal tests plus recovery tests: torn tails, corruption, uncommitted transactions, repeated recovery. |
| Multi-process control plane | REAL processes, REAL framed transport | `multiprocess.*`: a real `rg_backend_worker.exe` process, a real loopback socket, real epoch and boot-id fencing, real process kill and restart. |
| Coordinator restart | REAL processes | `multiprocess.coordinator_process_restart_through_rgctl`: apply, inspect, recover, revalidate and re-apply are five separate `rgctl` process runs against one durable journal and one live worker. |
| Physical rate enforcement on an interface | **UNSUPPORTED / NOT IMPLEMENTED** | There is no physical backend. `SyntheticDevice` is a software table with a readback; it is labelled SYNTHETIC in its descriptor, its class comment, its tool output and this report. |
| Packet rate, shaping accuracy, pacing, queue behaviour | **NOT MEASURED** | Out of scope; no data plane exists here. |
| NIC/DPU/switch/RDMA/optical behaviour | **NOT TESTED** | Nothing in this repository touches such hardware. |

## 4. Multi-process evidence

`tests/test_multiprocess.cpp` spawns real child processes and asserts on their
exit codes and captured output:

1. `apply_and_readback_cross_a_real_process_boundary` - a coordinator applies an
   envelope through `RemoteBackend` to a worker process; the readback confirms
   it; byte, frame and round-trip counters prove the claim crossed a socket; the
   worker's own exit report corroborates the boot id and apply count.
2. `worker_death_leaves_the_effect_unknown` - the worker is killed; the engine
   withdraws the session admission, demotes the effect, refuses to re-apply and
   reports `BackendSessionLost`.
3. `worker_restart_requires_revalidation_and_a_fresh_apply` - the worker is
   replaced by a fresh incarnation with a new boot id; revalidation finds nothing
   enforced, requeues the envelope, and a fresh apply re-establishes it.
4. `stale_epoch_and_boot_are_fenced_by_the_worker` - an older epoch is refused, a
   conflicting boot id at the same epoch is refused, and a strictly newer epoch
   is admitted after clearing the device table.
5. `malformed_frames_are_refused_by_a_real_worker` - raw garbage, an oversized
   declared length and a truncated frame are all refused, and the worker still
   serves a clean session afterwards.
6. `coordinator_process_restart_through_rgctl` - five separate coordinator
   processes; the durable log shows no uncommitted records at the end.

## 5. Persistence, restart and fencing evidence

* Every mutation is committed with a durability barrier before it is
  acknowledged; `journal*` tests assert that what the engine acknowledged is
  already on disk and replayable.
* Recovery never restores liveness: `RecoveryReport::liveness_restored` is always
  `false`, the backend is not bound, and every effect claim is demoted.
* Recovery always advances the fabric epoch; the previous incarnation's actor
  context is fenced (`recovery.durable_state_is_restored_but_effect_is_demoted`).
* Unfinished attempts become `ambiguous` and can never publish success
  (`recovery.unfinished_attempts_become_ambiguous_and_cannot_succeed`).
* Repeated recovery never escalates a claim to truth
  (`recovery.repeated_recovery_never_escalates_a_claim_to_truth`).
* A torn journal tail is discarded and reported, and can be compacted away
  (`journal.torn_tail_is_dropped_and_reported`, `recovery.truncated_tail_is_truncated_on_request`).

## 6. Lock / re-entrancy / shutdown audit

Audited explicitly:

| Hazard | Finding |
| --- | --- |
| Read-to-write re-entry on the same lock | The engine takes one non-recursive mutex and never calls a public method while holding it. Emission is deferred until the outermost unlock (`src/engine_lock.hpp`). |
| Callbacks or event emission under internal locks | Structurally impossible for the engine: `ScopedLock` defers emission. Proven by `concurrency.event_sinks_may_re_enter_the_engine`, whose sink calls `list_envelopes` from inside the callback. |
| Backend I/O under the lock | Never: dispatch, revoke and readback run outside the lock in a snapshot/revalidate/commit sequence. |
| Mutex re-entry | No lock is recursive; no engine method re-enters another public method under the lock. |
| Shutdown or join while holding state workers need | `WorkerSession::run` joins its pool holding neither the queue mutex nor the state mutex; the listener signals every live session before joining any session thread, so a join can never wait on a thread blocked on a healthy socket. |
| Reversed lock ordering | Documented order: `queue_mutex_` / `write_mutex_` may be held while taking `state_mutex_`; nothing acquires them in the opposite order. The registry mutex is never held while joining or while calling into a session's socket. |
| Re-entrant progress callbacks | Not present: no progress callback exists. The only embedder callback is the event sink, which is deferred. |
| Shutdown paths that prevent awaited work from completing | Cancelled work cannot report success: work that arrives after the stop signal is never started, and in-flight attempts become `abandoned`/`ambiguous` with revalidation required. |

## 7. Defects found and fixed

Every item below was found by the validation effort itself, not by inspection
alone, and each fix is covered by a test.

1. **`EngineConfig::initial_epoch` was ignored.** The engine always started at
   epoch 1, so a process configured for epoch N would be fenced by its own past
   or could appear older than it is. (Severity: high - incarnation fencing.)
2. **Cross-process requests carried the wrong incarnation.** `RemoteBackend` sent
   the worker's boot id while the device fenced on the coordinator's, so every
   remote apply was rejected as a boot mismatch. Requests now carry the
   coordinator incarnation; responses carry the enforcement incarnation.
3. **A worker admitted its own boot id at construction**, so the first legitimate
   coordinator claiming the same epoch was refused. A device driven by a remote
   coordinator now starts with nobody admitted: fail closed.
4. **`bind_backend` trusted the caller's epoch claim.** A backend that reports no
   session epoch at all could be admitted. Now both the caller's claim and the
   backend's own report must agree with the current epoch.
5. **A backwards clock could mint burst.** `refill_bucket` moved its anchor to the
   regressed instant, which would have re-priced the interval once the clock
   caught up. The anchor is now kept and the anomaly reported.
6. **Saturation was invisible.** `RefillOutcome::tokens_granted` reported the
   computed grant before the capacity clip; it now reports what the balance
   actually received.
7. **Audit events could be emitted under the state lock** on early-return paths,
   which would deadlock a re-entrant sink. Fixed structurally, not by convention.
8. **A worker could hang joining a session blocked on a healthy socket**, and
   closing the listening socket from another thread did not reliably cancel a
   blocking `accept()`. Fixed with a session registry, explicit stop signalling
   and a wake-up connection.
9. **`shutdown()` alone did not reliably wake a blocked receive** in the session
   stop path; `Socket::close()` is now idempotent and thread-safe and is used to
   cancel session I/O. Two test hangs were traced to this and are gone.
10. **A coordinator that merely exited stopped the enforcement worker**, because
    `~RemoteBackend` sent a shutdown request. Observed as a recovery failure in
    the multi-process rgctl flow. Destruction now drops the session; stopping the
    worker is explicit.
11. **A vanished effect was never reconsidered.** `revalidate_all` only looked at
    envelopes already flagged as requiring revalidation, so an effect that
    disappeared underneath the engine stayed "applied". The scheduled
    revalidation horizon is now a selection trigger, and
    `lifecycle.an_effect_that_vanishes_is_caught_by_revalidation` covers it.
12. **Static analyzer finding C28020** (unprovable index bound in the CRC-32C
    table). Fixed by using a plain array with a masked index; the analyzer is now
    silent.
13. **`compensating_revoke_required` was not propagated** to `ApplyDispatch`, so a
    caller could not see that a readback had exceeded the authorized ceiling.
    Now propagated, and the reason that invalidated the envelope is kept on the
    envelope rather than being overwritten by the compensating action.

Where a test expectation was wrong rather than the product (for example the
deterministic tie-break between equal authorities, or the exact value of a
window's start), the test was corrected and the rule documented; those are not
counted above.

## 8. Install and downstream consumer

    cmake --install build/release --prefix <prefix>
    cmake -S examples/consumer -B <dir> -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build <dir> && <dir>/consumer

Recorded output:

    consumer: envelope env:1/gen:1 state=APPLIED reason=ApplyVerified ceiling=4000
              target=2000 floor=500 burst=8000 burst_remaining=8000 applied=4000
              (verified) enforceable_now=yes action=NONE
    consumer: Rate Governor 1.0.0 usable from an installed package
    consumer: burst remaining after spending 100 = 7900

The consumer is a separate CMake project that is not part of this build; it links
`RateGovernor::rate_governor` found through the exported package and compiles with
`/W4 /WX`.

## 9. Benchmarks

See `docs/BENCHMARKS.md`. All figures there are labelled SYNTHETIC and measure
completed control-plane operations only.

## 10. Fresh clone closure

The release is validated from committed sources only:

    git clone <repository> <fresh>
    cd <fresh>
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    build/tests/rg_tests

The working tree is not used by this procedure; see `13` for the recorded result.

## 11. Not validated here (stated plainly)

* Physical enforcement of any rate on any interface. No physical backend exists
  in this release.
* Packet rate, throughput, pacing accuracy, jitter, queue occupancy.
* NIC, DPU, switch, RDMA, NVLink, optical or any multi-node behaviour.
* GCC and Clang builds, and any non-Windows platform: the sources contain the
  documented POSIX guards, but they were not exercised on this host.
* Long-duration soak testing. The suite is thorough per scenario but is not a
  soak test.
* Throughput of the multi-process path: it is validated for correctness, not
  speed.
* The journal format is versioned (`kJournalFormatVersion`) and the reader rejects
  versions it does not understand, but no forward-compatibility work exists for a
  version that does not exist yet.

## 12. Repository hygiene

* No TODO, FIXME, XXX, HACK or placeholder markers in first-party sources.
* No machine-specific paths, no debug prints outside `--verbose` tool output, no
  abandoned scripts, no generated artifacts in the tree.
* `git status --porcelain` at closure contains only the intended sources; build
  trees are ignored.
* LICENSE contains the full Apache License 2.0 text with the copyright notice
  `Copyright 2026 Summon Software Labs`.
* README describes implemented reality only and ends with the required license
  section.

## 13. Fresh clone result

Recorded from the release commit (see the tag message and final report for the
exact commit): a clone of the committed sources configured, built and passed the
full test suite, with no working-tree files involved. The procedure in `10` is the
one that was executed.
