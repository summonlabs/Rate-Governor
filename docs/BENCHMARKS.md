# Synthetic control-plane benchmarks

**Label: SYNTHETIC.** These numbers measure completed software control-plane
operations per second on one machine. They are not packet rates, not link
throughput, not shaping accuracy, and not a property of any network device. Rate
Governor does not shape any interface in this release, so no packet-rate claim is
made or implied anywhere below.

Reproduce with:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/rg_bench --iterations 2000 --journal bench.rgjournal
    ./build/rg_bench --iterations 2000 --json          # machine-readable

## Environment

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 7 9800X3D, 8 cores / 16 threads, 4.7 GHz max |
| OS | Microsoft Windows 11 Pro, 10.0.26200 |
| Toolchain | MSVC 19.44.35222 (Tools 14.44.35207), CMake 4.3.2, Ninja |
| Build | Release (`/O2`), C++20, strict warnings |
| Journal | local NTFS file, durability barrier per transaction |

## Measurements

Two consecutive runs, 2000 completed operations each. "Ops" counts **completed**
work; nothing here measures enqueue latency.

| Measurement | Run A | Run B | Unit |
| --- | --- | --- | --- |
| `token_bucket_refill` - exact refill plus one consume | 35,714,285 | 19,980,019 | completed refill+consume per second |
| `plan_derivation` - full `derive_plan` with fingerprint and invariant checks | 2,238,137 | 1,332,445 | plans per second |
| `authorize_apply_verify_revoke` - open, authorize, dispatch, read back, verify, revoke, verify, in-process synthetic backend, no journal | 33,303 | 33,541 | full cycles per second |
| `durable_apply_verify_revoke` - the same cycle with a strict durable journal | 164 | 166 | full cycles per second |

### What the numbers mean

* **Plan derivation** is pure computation: authority checks, minimum-of-authority
  ceiling, clamping, fingerprint fill and two consistency proofs. A million-plus
  plans per second is a software figure for one core; it says nothing about
  traffic.
* **The in-process lifecycle** includes a real readback of the synthetic device
  for every apply and every revoke. It is dominated by state management and
  exact-arithmetic checks, not by the device.
* **The durable lifecycle** is bounded by the platform durability barrier
  (`FlushFileBuffers`) on every transaction, not by CPU work: roughly 6 ms per
  transaction on this machine, with several transactions per cycle. This is the
  honest cost of "never acknowledge before durable". An embedder that needs more
  throughput on the durable path should batch its own control-plane operations,
  not weaken the barrier.
* **Token bucket refill** is the exact integer path
  (`floor(elapsed_ns * rate / 1e9)` with a carried fractional numerator), so the
  figure also covers the exactness guarantee.

## What was deliberately not measured

* Packet processing, shaping accuracy, pacing jitter, queue occupancy, tail
  latency, or any data-plane property. There is no data plane here.
* Multi-node, multi-switch, RDMA, NVLink, optical, NIC or DPU behaviour. None of
  those were exercised, and no result here should be read as evidence about them.
* The multi-process path throughput. The tests exercise it for correctness
  (real worker process, real framed transport, real fencing), not for speed; a
  loopback round trip per operation would measure the loopback stack as much as
  Rate Governor.
