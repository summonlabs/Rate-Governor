# Rate Governor architecture

This document records what Rate Governor 1.0.0 actually is, how its decisions are
made, and where its boundary lies.

## 1. The question this component answers

Given

* an authoritative flow/resource binding,
* granted or reserved bandwidth,
* an explicit rate policy,
* a burst allowance,
* service obligations,
* current generations, and
* a named enforcement backend,

what rate envelope is **legally enforceable right now**, how much burst remains,
what effect has **actually been applied**, and when must the envelope be reduced,
revoked, revalidated, fenced, or rejected as stale?

Everything in this repository exists to answer that question deterministically,
explainably and durably. Nothing in it exists to decide who *should* get
bandwidth.

## 2. Boundary

Owned here:

* rate-envelope authority: what the funding authorities legally support;
* enforcement-state governance: attempt lifecycle, verification, revocation,
  staleness, fencing, revalidation;
* exact integer rate/burst arithmetic and its invariants;
* durable configuration, committed authoritative state and audit history;
* a narrow, verified backend abstraction and its framed transport.

Deliberately **not** owned here, and absent from the API:

* arbitration between flows, admission control, reservation creation;
* path selection, packet scheduling, queue disciplines, priority or QoS class
  semantics, congestion control;
* vendor device programming (only reachable through `IEnforcementBackend`);
* any claim about physical rate, throughput, pacing accuracy or device state.

## 3. Model

| Concept | Meaning |
| --- | --- |
| `RateEnvelopeId` / `Generation` | The governed object and its authoritative revision. |
| `FlowId`, `ResourceId`, `GrantId`, `ReservationId`, `PolicyId`, `BackendId` | Strongly typed identities; a generation is carried alongside each. |
| `FabricEpoch` | Incarnation-wide fencing epoch. Advancing it invalidates every prior attempt and effect claim. |
| `WorkerBootId` | Incarnation identity of an enforcement process (the process that holds effect and answers readbacks). |
| `EnforcementAttemptId` | One apply or revoke attempt, with its own idempotency key. |
| `Provenance` | Who asserted a fact: operator, configuration, recovery, backend report, or derived. `Unknown` is never positive authority. |

Entities: `Flow`, `Resource`, `Grant`, `Reservation`, `Policy`,
`BackendDescriptor`, `RateEnvelope`, `EnforcementAttempt`.

Magnitudes are **integers only**. There is no floating point anywhere in an
authoritative decision.

* rate: tokens (rate units) per second, `u64`;
* burst: tokens, `u64`;
* time: nanoseconds, `u64`, always supplied explicitly by an `IClock`;
* every combination goes through the checked helpers in
  `include/rate_governor/checked_math.hpp` (`mul_div_mod`, `checked_add`, ...),
  which either produce an exact result or report failure. They never wrap.

## 4. Separations the code enforces

| Separation | How it is enforced |
| --- | --- |
| Grant is not enforcement | A grant only funds a ceiling; the envelope is what gets enforced. |
| Configured rate is not applied rate | `EnvelopeView` exposes `effective_*` (authorized) and `applied_*` (read back) as distinct fields. |
| Backend acknowledgement is not effect | `finish_apply` records the acknowledgement, then performs a separate readback; only a matching readback sets `effect_verified`. |
| Burst allowance is not permanent capacity | Burst tokens live in a bucket bounded by `effective_burst_tokens`; they never raise the ceiling. |
| Stale funding is not authority | Any generation change, state change or window expiry invalidates the envelope; a stale envelope can never be re-authorized. |
| Verified once is not verified now | Every effect carries the epoch and boot id that produced it, plus a scheduled revalidation horizon. |

## 5. Plan derivation

`derive_plan(PlanInputs, now)` in `src/plan.cpp` is a pure function. It:

1. rejects a missing, mismatched, inactive or expired authority with a specific
   `ReasonCode`;
2. computes `effective_ceiling = min(grant, reservation?, policy, resource,
   backend_max?)` and names the **limiting authority**;
3. rejects a policy whose floor exceeds the funded ceiling: it never silently
   breaks `floor <= target <= ceiling` and never raises the ceiling above what
   funds it;
4. clamps the target into `[floor, ceiling]` and records that it did so;
5. takes the burst allowance as the minimum of every authority that declares one
   (`0` means "not declared", not "zero");
6. intersects the validity windows;
7. fills a `PlanFingerprint` with every generation **and** every raw magnitude
   the decision used;
8. verifies `fingerprint_self_consistent` and `invariants_hold` before
   returning.

`fingerprint_matches_authority` re-checks a stored fingerprint against the
current authority, which is how a decision is proven to still be the decision
that was justified.

## 6. Enforcement lifecycle

    desired --> authorized --> dispatching --> applied
                    ^              |              |
                    |              | readback     | readback above
                    |              v  mismatch    v the authorized ceiling
                    |          degraded <---------+
                    |              |
                    +--- retry ----+
                                   v
                        revoke-pending --verify--> revoked

`stale` and `failed` are terminal for that binding: new funding means a new
envelope.

Attempt states: `created -> dispatched -> acknowledged -> verified`, or
`failed`, `cancelled`, `abandoned`, `ambiguous`, `late-rejected`,
`duplicate-ignored`.

Invariants enforced by construction and checked by tests:

* only `verified` may publish success; `attempt_state_may_publish_success` is the
  single predicate that gates it;
* a cancelled, abandoned, ambiguous or late attempt can never publish success
  afterwards;
* duplicate apply/revoke and duplicate completions are idempotent, keyed by
  `(envelope, envelope generation, kind)`;
* `applied` implies `effect_verified`, and `requires_revalidation` implies
  `!effect_verified`;
* burst accounting never overflows and never goes negative.

## 7. Races closed

| Race | Closure |
| --- | --- |
| Grant recall racing apply | The completion is re-checked against current generations after dispatch; a mismatch is a late rejection and the envelope moves to revoke-pending with revalidation required. |
| Policy or capacity change during apply | Same re-check; the envelope keeps the reason that invalidated it. |
| Delayed backend completion | Attempt deadlines turn a silent attempt ambiguous; a late completion is rejected and never verifies. |
| Worker death | `mark_session_lost` (detected by the engine, or reported by `tick`) withdraws the session admission and demotes every effect claim it supported. |
| Coordinator restart | Recovery advances the epoch, marks unfinished attempts ambiguous, and demotes every effect claim to "requires revalidation". |
| Duplicate completion | Idempotent: the second arrival is `duplicate-ignored` and changes nothing. |
| Clock regression and tick overflow | Refill never mints tokens from a backwards clock and never moves the anchor; absurd elapsed time saturates at capacity. |
| Burst boundary | Exact fractional carry: split intervals agree with whole intervals to the token. |
| Malformed backend report | Every frame is bounded and CRC-checked; a response carrying a foreign attempt id, boot id or epoch is refused. |

## 8. Locking discipline

One state mutex guards all authoritative state. The rules are structural, not
conventional:

* **No callback under the lock.** `ScopedLock` (src/engine_lock.hpp) tracks the
  per-thread nesting depth; an event emitted while a lock is held on this thread
  is deferred until the outermost lock is released. A sink may therefore
  re-enter the engine for read-only queries, and
  `concurrency.event_sinks_may_re_enter_the_engine` proves it.
* **No backend I/O under the lock.** Dispatch, revoke and readback happen in a
  two-phase *snapshot -> call -> revalidate -> commit* sequence with the lock
  released in between. Only cheap, non-blocking session predicates
  (`session_boot`, `session_epoch`, `session_live`) may be called under it, and
  they must not re-enter the engine; this is stated in `backend.hpp`.
* **Write-ahead before memory.** A mutation computes the values to persist,
  writes and commits its transaction durably, and only then mutates memory. A
  failed commit leaves memory untouched and the engine refuses further mutations
  (`durability_degraded`).
* **Lock ordering in the worker.** `queue_mutex_` and `write_mutex_` may be held
  while taking `state_mutex_`; nothing takes them in the other order. The
  session registry mutex is never held while joining a thread or while calling
  into a session's socket.
* **Shutdown without inversion.** `WorkerSession::run` stops the reader, closes
  the session queue, joins the pool (holding no lock the pool needs), then closes
  the socket. The listener signals every live session before joining any session
  thread, so a join can never wait on a thread that is still blocked on a healthy
  socket.

Re-entrancy, callback-under-lock, reversed ordering and shutdown-join hazards
were audited explicitly; see `docs/VALIDATION.md` section 6.

## 9. Durability

Journal framing: a fixed 36-byte header (magic `RGJ1`, format version, record
type, flags, sequence, transaction id, payload length, CRC-32C over the
header-without-CRC plus the payload) followed by the payload.

Protocol per mutation: **validate -> bind authority -> plan -> reserve ->
journal/temp state -> perform work -> verify -> commit -> retire**.

* Records are grouped in explicit transactions; a record is authoritative only
  when its transaction's commit record is present.
* `commit()` flushes and calls the platform durability barrier
  (`_commit`/`fsync`) before reporting success. Nothing is acknowledged before
  its state is durable.
* Recovery replays only committed records, discards an uncommitted tail, and
  reports a torn tail instead of repairing it.
* Compaction writes a full snapshot to a temporary file, barriers it, and
  atomically renames it over the log; the live log is never partially written.
* Payload sizes, file size and scan size are all bounded; an implausible journal
  is refused rather than allocated for.

Recovery classification:

| Class | Treatment |
| --- | --- |
| Durable configuration (flows, grants, reservations, policies, resources, backends) | Restored as configuration. |
| Committed authoritative state (envelopes, attempts) | Restored as history. |
| Unfinished attempts | Ambiguous. They can never publish success. |
| Effect claims | Demoted: `effect_verified = false`, `requires_revalidation = true`. |
| Liveness, sessions, leases, publisher authority | **Never** restored. `RecoveryReport::liveness_restored` is always `false`. |

## 10. Transport and the worker process

* Loopback TCP only; framing is magic-tagged, version-tagged, length-bounded
  (64 KiB) and CRC-32C protected.
* A worker admits a coordinator incarnation per fabric epoch: an older epoch is
  fenced, a different boot id at the same epoch is fenced, and a strictly newer
  epoch advances the device's epoch and **clears** its enforcement table: an
  epoch advance means no prior enforcement decision is current.
* A worker serves concurrent sessions from one shared device; each session is one
  connection with its own bounded request queue and worker pool.
* The coordinator's `RemoteBackend` sends its own incarnation in requests and
  expects the worker's incarnation in responses; a response from a foreign
  incarnation is refused.

## 11. Backends shipped

| Backend | Kind | Behaviour |
| --- | --- | --- |
| `SyntheticBackend` / `SyntheticDevice` | SYNTHETIC | Software table plus readback. Enforces nothing on any interface. |
| `UnsupportedBackend` | UNSUPPORTED | Refuses everything; can never support an applied claim. |
| `RemoteBackend` | SYNTHETIC effect, REAL transport | The same synthetic device, driven across a real OS socket from a real worker process. |

There is no physical backend implementation in this release, and no physical
validation claim anywhere in the repository.

## 12. Determinism

* No wall-clock reads inside authoritative decisions; the clock is injected.
* No randomness: failure injection is counter-based, and the randomized test
  generator is seeded and reproducible from its seed alone.
* Explanations are generated from the same values the decision used, and are
  bounded and marked when truncated.
* `inspect`, `describe` and the audit stream expose the same facts, so a decision
  can be replayed and audited after the fact.
