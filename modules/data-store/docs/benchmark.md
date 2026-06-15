# data-store benchmark

Throughput + latency of `ds-server` over the AF_UNIX socket, via the public
`data_store::Client` API. Tool: `modules/data-store/bench/ds_bench.cpp`.

## How to run

```sh
# In the build env (ACE on the link line). One-liner used for the numbers below:
g++ -std=c++17 -O2 -Imodules/data-store/inc -I$ACE_ROOT/include \
    modules/data-store/bench/ds_bench.cpp libdatastore_client.a \
    -L$ACE_ROOT/lib -lACE -lpthread -o ds-bench
# or: cmake modules/data-store -DBUILD_DS_BENCH=ON && ninja ds-bench

ds-server --socket=/run/iot/data_store.sock --schema-dir=<dir> --persist-dir=<dir> &
ds-bench socket=/run/iot/data_store.sock n=20000 vsize=64 batch=50 pid=$!
```

`pid=<ds-server-pid>` lets the bench read the server's `/proc/<pid>/status`
(RSS + peak `VmHWM`). At the end the bench writes a JSON summary to the ds key
**`ds.bench.summary`** (`config`, `mem`, and per-op `{ops,p50,p95,p99}`), so the
latest result is queryable: `ds-cli get ds.bench.summary` (or surfaced in the UI).

## Results (indicative)

Environment: containerized Linux (podman VM, 5 vCPU) — **not** target hardware.
Single client thread; each op is a synchronous request→response round-trip.
`n=20000`, 64-byte values, batch=50.

| Op | throughput | mean | p50 | p95 | p99 | max |
|----|-----------:|-----:|----:|----:|----:|----:|
| `set` (persist + fsync) | 10.1k ops/s | 98 µs | 67 µs | 428 µs | 688 µs | 3.2 ms |
| `set` (volatile, no fsync) | 14.0k ops/s | 71 µs | 67 µs | 112 µs | 159 µs | 2.0 ms |
| `get` | 15.6k ops/s | 64 µs | 66 µs | 100 µs | 143 µs | 1.6 ms |
| `set` (batch ×50) | **248k keys/s** | 198 µs/call | 119 µs | 154 µs | 207 µs | 31 ms |
| `watch`→notify delivery | — | 737 µs | 732 µs | 774 µs | 847 µs | 4.8 ms |

**ds-server memory:** RSS **~5.2 MB → ~6.7 MB** after the run (≈ +1.5 MB holding
~1000+ keys), peak `VmHWM` ~6.7 MB. A small, flat footprint — fine for an RPi.

## Reading the numbers

- **Single-client throughput is round-trip-bound, not server-bound.** `get` at
  ~64 µs/op ⇒ ~15.6k ops/s is essentially `1 / round-trip-latency`. The server
  ceiling is much higher with concurrent clients (see Limitations).
- **fsync is the write cost.** Persistent `set` (write-through + `fsync` per
  call) is ~30 % slower than volatile here. On real flash (RPi SD) `fsync` is
  **far** costlier than in this VM — expect persistent single-`set` throughput to
  drop sharply on-device.
- **Batching wins big.** A 50-key `set` lands ~248k keys/s (~2.4 µs/key
  amortised) — it pays the round-trip + one `fsync` once for the whole batch.
  **Recommendation:** group multi-key writes into one `set({pairs})` (the daemons
  already do this in several places); avoid per-key `set` loops on hot paths, and
  use `set_volatile` for ephemeral state.
- **Watch/notify latency is sub-ms** (p50 ~0.7 ms) — comfortable for config-change
  propagation and the UI long-poll wakeups.

## Limitations / follow-ups

- **Not target hardware.** Run on the cloud VM and an RPi for absolute numbers;
  the SD-card `fsync` profile in particular will differ from this VM.
- **Single client.** ds_bench drives one connection. A concurrent-client variant
  (N threads/connections) would measure the *server's* aggregate ceiling rather
  than per-connection round-trip latency — the more useful number for a fleet.
- Value size fixed at 64 B; large values (certs/JSON blobs) would show framing +
  copy cost — add a `vsize` sweep.
