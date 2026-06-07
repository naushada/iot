# L22 — TDD: Per-Daemon CPU & Memory Telemetry

> Companion to `log/L22/design.md`. Test-first plan for `StatsPublisher`,
> the `services.lua` schema, daemon wiring, and the cloud-UI Services table.
> Tests build and run via podman against the toolchain image
> `localhost/iot-httpd:lp` (gcc/g++, cmake, ACE 7.0.0, gtest, Lua 5.3).

## 0. How to run the tests (podman)

The gtest binary `ds-tests` builds out-of-tree against the working copy:

```bash
podman run --rm -v "$PWD":/src:ro localhost/iot-httpd:lp bash -lc '
  cmake -S /src/modules/data-store -B /tmp/build -DBUILD_DATA_STORE_TESTS=ON &&
  cmake --build /tmp/build --target ds-tests -j4 &&
  /tmp/build/ds-tests --gtest_filter="Stats*:Services*"'
```

Read-only source mount + container-local `/tmp/build` keeps the host tree
clean. The same image already builds the rest of `ds-tests` (verified).

## 1. Unit under test

`data_store::StatsPublisher` and its `stats_detail` free functions
(`modules/data-store/inc/data_store/stats_publisher.hpp`,
`modules/data-store/src/stats_publisher.cpp`).

Design split that makes it testable without a reactor or a live ds:
- **Pure parsers / math** in `stats_detail` — fed fixture directories via
  `StatsRoots{cgroup_root, proc_self}`; no ACE, no Client.
- **`sample()`** — composes the parsers + CPU delta using `steady_clock`.
- **timer/thread** (`open()/close()/handle_timeout()`) — exercised by one
  short integration test against an in-process ds-server (the
  `LogBufferTest` harness pattern).

## 2. Test file

`modules/data-store/test/stats_publisher_test.cpp`, added to the `ds-tests`
target (and `src/stats_publisher.cpp` to its sources) in
`modules/data-store/CMakeLists.txt`.

### 2.1 Fixtures (temp dirs, `mkdtemp` like `log_buffer_test.cpp`)

A helper writes a fake cgroup + proc tree:

```
make_cgroup_v2(root, {usage_usec, mem_bytes, pids})
   root/cgroup.controllers          "cpu memory pids\n"
   root/cpu.stat                    "usage_usec 1500000\n..."
   root/memory.current              "<mem_bytes>\n"
   root/pids.current                "<pids>\n"

make_cgroup_v1(root, {usage_ns, mem_bytes, pids})
   root/cpuacct/cpuacct.usage       "<usage_ns>\n"
   root/memory/memory.usage_in_bytes
   root/pids/pids.current
   (no root/cgroup.controllers)

make_proc_self(root, n_fds)         creates root/fd/ with n symlink/files
```

### 2.2 Cases (red → green)

**Parsers (v2)**
1. `detect_cgroup` returns `v2` when `cgroup.controllers` exists.
2. `parse_mem_kb` v2: `memory.current = 41943040` → `40960` (bytes/1024).
3. `parse_pids`   v2: `pids.current = 7` → `7`.
4. `parse_cpu_usec` v2: reads the `usage_usec` line of `cpu.stat`.

**Parsers (v1 fallback)**
5. `detect_cgroup` returns `v1` when no `cgroup.controllers` but
   `cpuacct/cpuacct.usage` exists.
6. `parse_mem_kb` v1 from `memory/memory.usage_in_bytes`.
7. `parse_pids`   v1 from `pids/pids.current`.
8. `parse_cpu_usec` v1 reads ns from `cpuacct.usage` and normalizes to µs.

**CPU math (pure)**
9. `cpu_permille(prev=1'000'000, now=1'500'000, dt=1.0)` → 500000 µs over
   1 s = 0.5 host-core-second → **500 ‰** (50.0 %).
10. `cpu_permille(prev=0, now=2'000'000, dt=1.0)` → 2.0 cores → **2000 ‰**.
11. `cpu_permille(now < prev /* counter reset */, …)` → clamped **0**.
12. `cpu_permille(dt <= 0)` → **0** (no divide-by-zero).

**FD count**
13. `count_fds` returns the number of entries in `fd/` (excludes `.`/`..`).
14. `count_fds` on a missing `fd/` dir → **0** (graceful).

**`sample()` behaviour**
15. First `sample()` returns `cpu_permille == 0` (baseline only) but correct
    `mem_rss_kb`, `fd_count`, `threads`.
16. Second `sample()`, after rewriting `cpu.stat` with +500000 µs and ~the
    test's elapsed wall time, returns a CPU permille in the expected band.
    (Tolerate timing: assert `> 0` and `<= 1000 * ncpu`; the exact-math
    assertion is covered by case 9–12 on the pure helper.)
17. `StatsRoots` pointing at an empty dir (`Cg::none`) → all-zero sample,
    no throw.

**Publish (sink — no server, no reactor)**
18. Construct `StatsPublisher("services.test", sink, fixture_roots)` where
    `sink` captures the `std::vector<KV>` into a local. Call
    `handle_timeout(ACE_Time_Value::zero, nullptr)` directly (simulates a
    timer fire). Assert the captured batch contains
    `services.test.cpu.permille`, `…mem.rss.kb`, `…fd.count`, `…threads`
    (all int32) **and** `services.stats.version` (int32, non-zero). This
    needs neither a ds-server nor a running reactor — the sink decoupling is
    exactly what makes it a pure unit test.
19. Two `handle_timeout` fires with `cpu.stat` advanced between them →
    second batch carries a `cpu.permille > 0`.

**Timer / thread lifecycle (ACE active object)**
20. `open(1)` then `close()` fires the sink ≥1 time within ~1.3 s (sink
    increments an atomic counter); `close()` joins the ACE_Task thread.
    `close()` is idempotent and safe before `open()`; dtor after `open()`
    without explicit `close()` still joins. To avoid contaminating the
    shared `ACE_Reactor::instance()` used by other ds-tests (which pump it
    via `handle_events`), this case calls
    `ACE_Reactor::instance()->reset_reactor_event_loop()` after `close()`.
    Kept minimal — the timer wiring mirrors `Worker`'s active-object pattern,
    so the heavy assertions live in the pure/`sample()`/publish cases above.

## 3. Schema test

Extend `modules/data-store/test/schema_test.cpp` (or a focused
`Services*` case): load `schemas/services.lua`, assert for a representative
service (e.g. `services.cloud.iot.cloudd`) the four keys exist with
`type == "integer"`, `min == 0`, `access == "Viewer"`, and that
`services.stats.version` exists as a Viewer integer. (If the existing schema
test harness only checks load-success, add a minimal key-presence assertion
following its current style.)

## 4. Daemon wiring (no new unit tests — covered by build + smoke)

Each daemon constructs `StatsPublisher` after `connect()` and calls `open()`;
verified by the existing per-daemon build and the `test-full-http.sh` /
compose smoke path. A post-wiring manual check:

```bash
# in a running cloud stack
ds-cli get services.cloud.iot.cloudd.cpu.permille
ds-cli get services.cloud.iot.cloudd.mem.rss.kb
ds-cli get services.stats.version    # increments every ~10s
```

## 5. Cloud-UI Services component test

`apps/cloud/ui` (Angular + Jasmine/Karma). Update/extend the
`ServicesListComponent` spec:
- Renders a `clr-datagrid` (not the old `.svc-grid`).
- Given a mocked `dbGet` returning `services.<svc>.cpu.permille = 123`,
  `…mem.rss.kb = 40960`, `…fd.count = 18`, `…threads = 6`, the row shows
  `12.3 %`, a humanized `40.0 MB`, `18`, `6`.
- `scheduleLongPoll()` long-polls `services.stats.version`.

(UI test runs in the Node UI image, not the C++ toolchain image.)

## 6. Definition of done

- `ds-tests --gtest_filter="Stats*:Services*"` green in podman.
- `services.lua` carries the new keys; schema test green.
- All daemons construct + `open()` a `StatsPublisher`; builds green.
- Cloud-UI Services renders the datagrid with the four metric columns and
  long-polls `services.stats.version`.
