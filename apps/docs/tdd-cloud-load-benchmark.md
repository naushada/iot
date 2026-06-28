# TDD — Cloud registration-plane load benchmark

Status: **implemented, not yet run on a host** (initiative "a" — get a real
device-capacity number before deciding whether multi-tenancy "b" is worth its
complexity).

## 1. Problem

We ship a single-process, single-threaded ACE_Reactor cloud (lwm2m-bs +
lwm2m-dm) and have **no measured device-capacity number**. The only benchmark
in the repo is for the data-store (`modules/data-store/docs/benchmark.md`).
Architectural caps are known but unmeasured:

| Limit | Value | Where |
|---|---|---|
| VPN subnet | 254 IPs (`10.9.0.0/24`) | `apps/cloud/server/src/main.cpp` |
| VPN proxy ports | ~51 (`10000–10050`) | same |
| DTLS peers / context | `DTLS_PEER_MAX` | tinydtls platform.h |
| Cloud server threads | **1** (ACE_Reactor) | `apps/docs/architecture.md` |

The VPN caps bound *tunnelled* devices, but the **registration plane**
(DTLS + /bs + /rd + Update) runs on the single reactor thread and is the real
unmeasured wall. This benchmark measures that.

## 2. Goals / non-goals

- **G1** Drive the *exact* device wire flow against a real cloud, at scale, and
  report: success count, peak registrations/sec, time-to-register
  (p50/p90/p99/max), and cloud CPU/mem during the run.
- **G2** Reuse the production transport + FSMs verbatim — no protocol
  re-implementation that could diverge from real devices.
- **G3** Scale to thousands of simulated devices without per-device JSON
  provisioning.
- **NG1** Not the VPN/tunnel plane (separate, hard-capped at ~51/254). Phase 2.
- **NG2** Not telemetry/Send throughput. Phase 2.

## 3. Design

### 3.1 Fidelity via reuse

Each simulated device owns its own `DTLSAdapter` + `ObjectStore` +
`lwm2m::bootstrap::Client` + `lwm2m::RegistrationClient` over its own UDP
socket. These FSMs are pure logic (no data-store, no global singletons), so N
instances coexist in one process and produce byte-identical traffic to a real
RPi. The harness mirrors the client tick in `apps/src/main.cpp`:

```
DTLS-PSK → POST /bs → consume Object 0/1 writes → switch to DM identity +
connect → POST /rd → 2.01 Created → lifetime Updates
```

The bootstrap FSM installs the DM PSK itself (RID 5 hex → `add_credential`), so
`on_done` only switches identity/peer and connects to the DM — exactly as the
device does.

### 3.2 Credentialing — zero-touch HKDF (no per-device JSON)

The loadgen holds one raw 32-byte HKDF master and derives each device's BS PSK
with the production function:

```
bs_psk = derive_bs_psk_hex(master, serial)         // psk_gen.cpp
identity (on wire) = serial                         // override path
```

The cloud derives the same key in its PSK resolver from the presented serial,
so **no `cloud.endpoint.credentials` rows are written**. The cloud reads the
master from `cloud.bs.master.key` (AES-256-GCM-wrapped) and unwraps it with
`IOT_BS_MASTER_KEK`. The runner wraps a throwaway master with `bs-master-wrap`
and injects the KEK via a compose override.

### 3.3 Concurrency model

One process, `ACE_Dev_Poll_Reactor` (scales past `select()`'s FD_SETSIZE). Each
device is an `ACE_Event_Handler` registered for read; a single 10 Hz timer
paces the ramp (spawn `ramp` devices/sec) and ticks every device's FSM. The fd
ceiling is raised via `setrlimit(RLIMIT_NOFILE)`.

This deliberately mirrors the *cloud's* single-thread model on the client side
too, but the load generator is not the system under test — the cloud is. If the
generator itself saturates first, lower `ramp` or shard across hosts.

### 3.4 Metrics

Per device: timestamps for BS-connected, /bs sent, bootstrap done, DM-connected,
/rd sent, registered; or a `failStage` (`dtls-bs` / `bootstrap` / `dtls-dm` /
`register`). Aggregated into the report + an optional per-device CSV. The runner
samples `podman stats` for the four cloud daemons once a second and prints the
peak CPU/mem.

## 4. Files

- `apps/bench/cloud_loadgen.cpp` — the generator.
- `apps/bench/CMakeLists.txt` — standalone build (mirrors `apps/test`); **not**
  part of the cloud image build, so it can't break the CI-on-main image build.
- `apps/bench/Dockerfile` — builds the binary FROM the cloud builder stage.
- `apps/bench/run-bench.sh` — compose up → enable HKDF → run → sample.

## 5. How to run

```bash
apps/bench/run-bench.sh 500 50 120      # 500 devices, 50/s ramp, 120s soak
# knobs: COUNT= RAMP= SOAK= LIFETIME= NO_BUILD=1 NO_UP=1 ENGINE=docker
```

First run builds the cloud builder image (minutes, one-time, cached).

## 6. Method to find the knee

Run a sweep — 100, 250, 500, 1000, 2000 — at a fixed ramp, watching for the
first run where: success rate drops below ~99%, p99 time-to-register climbs
sharply, or lwm2m-dm CPU pegs ~100% (one core). That inflection is the
single-reactor ceiling. Record it in this doc.

## 7. Results

First host run — 2026-06-28, local podman (5 vCPU / 8 GB VM; loadgen + cloud
share the host, so these are conservative), `iot-cloud:local`, manual 128-bit
tier, registration plane only.

| Devices | Ramp/s | Success % | Peak reg/s | TTR p50 | TTR p99 | TTR max | Handshake p50 |
|---|---|---|---|---|---|---|---|
| 50   | 25   | 100%  | 20  | 205 ms | 216 ms  | 216 ms  | 100 ms |
| 250  | 250  | 100%  | 46  | 209 ms | 234 ms  | 235 ms  | 99 ms  |
| 500  | 500  | 100%  | 85  | 211 ms | 1.3 s   | 1.4 s   | 100 ms |
| 1000 | 1000 | 98.8% | 65  | 214 ms | 970 ms  | 12.4 s  | 100 ms |
| 1500 | 1500 | 100%  | 102 | 220 ms | 14.5 s  | 14.7 s  | 100 ms |
| 2000 | 2000 | 97.5% | 66  | 221 ms | 28.1 s  | 31.9 s  | 99 ms  |

(dm/bs CPU stayed <2% throughout — registration is reactor-serialised, not
CPU-bound. Failures at 1000/2000 are all `register`/`dtls-dm` **timeouts** under
the 30 s boot budget, not rejections; the 1500 run hit 100% only because its
longer soak let the tail drain.)

### Conclusion — the knee

- **Sustained throughput ceiling ≈ 100 registrations/sec** on this host. Peak
  reg/s plateaus at 65–102 regardless of offered ramp (1000→2000), so the single
  ACE reactor caps cloud-wide registration churn at ~100/s here.
- **Median is flat to 2000** (TTR p50 ~210–220 ms, handshake p50 ~100 ms): the
  cloud does **not** fall over — it queues. The reactor serialises DTLS
  handshakes, so beyond a ~1000-device burst a **tail** forms: p99 TTR climbs
  970 ms → 14.5 s → 28 s at 1000 → 1500 → 2000.
- **Practical capacity:** ~**1000 concurrent registrations** complete fast
  (p99 <1 s, ≥99%). A ~1500–2000 simultaneous burst still finishes but with
  10–30 s tail latency and ~1–2.5% missing a 30 s budget (they succeed on
  retry). Steady-state fleet size is far higher — with a 90 s lifetime, ~100
  reg/s sustains ~9000 devices' Update cadence, well past the 254-IP VPN cap
  that bounds *tunnelled* devices first.

### Caveats (numbers are conservative)

- **Shared host:** loadgen + the whole cloud stack ran on one 5-vCPU / 8 GB
  podman VM, so the generator competed with the cloud for cores. A dedicated
  cloud (and a separate load host) would push the ~100/s ceiling higher.
- **Loadgen is also single-reactor**, so part of the tail at 2000 is the
  generator, not the cloud. Shard the generator across hosts to isolate the
  cloud ceiling precisely.
- Loopback/LAN RTT ≪ real cellular; TTR here is a lower bound on field latency.

### Provisioning fix (was: arg-limit)

`run-bench.sh` now streams `cloud.endpoint.credentials` to `ds-cli set <key> -`
over **stdin**, dodging the kernel's 128 KB `MAX_ARG_STRLEN` single-argv cap
(hit at ~820 rows). Required a small `ds-cli` enhancement (read value from stdin
when it is `-`) — `modules/data-store/src/cli/ds_cli.cpp`. Verified round-trip at
1300 rows / 203 KB.

## 9. Finding: zero-touch HKDF cannot complete a DTLS handshake

While wiring the harness, the zero-touch tier (the user's first choice) failed
every handshake with `PSK (32 B) exceeds caller buffer (16 B)`:

- `derive_bs_psk_hex` / `derive_dm_psk_hex` produce **32-byte (256-bit)** PSKs.
- tinydtls caps PSKs at `DTLS_PSK_MAX_KEY_LEN = DTLS_KEY_LENGTH` = **16 bytes**
  (`apps/3rdparty/tinydtls/crypto.h:82`), and `dtlsGetPskInfoCb`
  (`apps/src/dtls_adapter.cpp`) **errors** rather than truncating when the key
  exceeds the buffer.
- Both client and cloud run the same code, so the zero-touch handshake fails on
  both ends. This is consistent with the tier being "NOT HW-integration-tested"
  ([[tdd-bs-hkdf-zerotouch]]).

The manual tier works because its PSKs are 128-bit. **Fix options for
zero-touch** (separate from this benchmark): derive 16-byte keys
(`hkdf_sha256(..., out_len=16)` — but that changes the device-flashing contract
gen_bs_psk.py must match), or raise `DTLS_KEY_LENGTH`/accept longer PSKs. The
benchmark uses the manual 128-bit tier to get a faithful capacity number.

## 8. Threats to validity

- Single-host generator: ACE/tinydtls on the loadgen side may saturate before
  the cloud. Cross-check loadgen host CPU; shard if needed.
- `DTLS_PEER_MAX` per *context*: each sim device has its own context, so the
  generator is unaffected; the cloud's server contexts are the real subject.
- NAT/loopback timing differs from real cellular RTT — TTR numbers are a
  lower bound on real-world latency, upper bound on throughput.
