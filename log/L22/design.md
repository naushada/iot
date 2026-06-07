# L22 — Per-Daemon CPU & Memory Telemetry

> **Status:** Draft — design phase. TDD to follow after approval.

## 1. Overview

Each iot daemon running inside its container self-reports its own CPU and
memory usage to the data store, exactly the way it already self-reports its
log output (`LogBuffer`) and ds-server reports `services.ds.uptime.sec`. The
cloud UI **Services** page then shows the live numbers as two extra columns,
converting the current card grid into a `clr-datagrid` table that reuses the
**Endpoints** page pattern.

```
┌──────────┐  cgroup +    ┌───────────────┐  long-poll  ┌──────────────┐
│  daemon  │  /proc/self  │ StatsPublisher│ ──────────→ │  ds-server   │
│ (cloudd, │ ───────────→ │  (lib class)  │  ds->set    │  services.*  │
│  httpd,  │   sample     └───────────────┘             └──────┬───────┘
│  lwm2m…) │   every 10s                                       │
└──────────┘                                            ┌──────┴───────┐
                                                        │  iot-httpd   │
                                                        │  /db/get     │
                                                        └──────┬───────┘
                                                               │
                                                    ┌──────────┴─────────┐
                                                    │ Cloud UI  Services │
                                                    │  clr-datagrid      │
                                                    │  + CPU% + Memory   │
                                                    └────────────────────┘
```

**Container-scoped self-instrumentation** — one daemon per container
(confirmed: ds-server / iot-cloudd / lwm2m-bs / lwm2m-dm / iot-httpd each run
as their own container in `apps/cloud/docker-compose.yml`), so per-daemon
usage *is* per-container usage. Each daemon reads its **own container's
cgroup** (`/sys/fs/cgroup/…`, mounted inside the container and scoped to that
cgroup) for CPU / memory / task count, and `/proc/self/fd` for the
descriptor count. No central collector, no daemon-to-daemon IPC, no
cross-namespace `/proc` access — everything flows through ds-server (see
`apps/cloud/CLAUDE.md` — "No HTTP between daemons").

Because cgroup CPU/memory/pids are accounted for the whole cgroup, any
**child process** the daemon spawns (e.g. openvpn-client → `openvpn(8)`,
wifi-client → `wpa_supplicant`) is automatically included in its container's
totals — no separate per-child sampling needed.

## 2. Schema (`modules/data-store/schemas/services.lua`) — L22

Typed numeric keys, four per daemon, slotted next to the existing
`.state` / `.uptime.sec` keys. Follows the `services.ds.uptime.sec`
precedent (typed `integer`, `min = 0`, `access = "Viewer"`).

```
services.<name>.cpu.permille   integer  default 0  min 0   -- parts-per-1000
                                                            --   123 = 12.3 %
services.<name>.mem.rss.kb     integer  default 0  min 0   -- resident set, KB
services.<name>.fd.count       integer  default 0  min 0   -- open file descriptors
services.<name>.threads        integer  default 0  min 0   -- live threads
```

`cpu.permille` (not `cpu.percent`) keeps the value an **integer** while
preserving one decimal of resolution — ds has no float type, and 12.3 % must
not round to 12. The UI divides by 10 for display.

Plus one global bump counter (mirrors `log.version`) so the UI long-polls a
**single** key instead of round-robining every daemon's two keys:

```
services.stats.version         integer  default 0           -- bumped on flush
```

### Per-daemon key set (mirrors existing service names)

Each service gets the same four-key set (`.cpu.permille`, `.mem.rss.kb`,
`.fd.count`, `.threads`):

| Service name (existing `.state` key) | New keys |
|--------------------------------------|----------|
| `services.ds`                  | `services.ds.cpu.permille`, `…mem.rss.kb`, `…fd.count`, `…threads` |
| `services.net.router`          | `…net.router.*` |
| `services.openvpn.client`      | `…openvpn.client.*` |
| `services.lwm2m.client`        | `…lwm2m.client.*` |
| `services.lwm2m.server`        | `…lwm2m.server.*` |
| `services.wifi.client`         | `…wifi.client.*` |
| `services.cloud.iot.cloudd`    | `…cloud.iot.cloudd.*` |
| `services.cloud.iot.httpd`     | `…cloud.iot.httpd.*` |
| `services.cloud.openvpn.server`| `…cloud.openvpn.server.*` |
| `services.cloud.lwm2m.bs`      | `…cloud.lwm2m.bs.*` |
| `services.cloud.lwm2m.dm`      | `…cloud.lwm2m.dm.*` |

Naming stays **dots-only** per `apps/cloud/CLAUDE.md`.

## 3. Reusable sampler — `data_store::StatsPublisher`

New lib class, sibling to `LogBuffer`, in the data-store module:

- `modules/data-store/inc/data_store/stats_publisher.hpp`
- `modules/data-store/src/stats_publisher.cpp`

**ACE-native, singleton reactor.** Per the project convention (ACE for
timers/threads, see `feedback_ace_logging`), `StatsPublisher` is an
`ACE_Event_Handler` whose `handle_timeout` does the sampling, driven by the
**`ACE_Reactor` singleton** (`ACE_Reactor::instance()`) — the reactor ACE
already provides; we reuse it rather than constructing a new one. Most iot
daemons block on `Client::recv_event` and never run the reactor loop, so
`StatsPublisher::open()` spawns **one** ACE thread (an `ACE_Task<ACE_MT_SYNCH>`)
that takes reactor ownership and runs `run_reactor_event_loop()`; the timer is
scheduled on that same singleton. `close()` cancels the timer and ends the
loop. (ds-server already runs the singleton reactor in `main` for
`UptimePublisher`, so when its self-report is wired in §7 it schedules the
timer only — no extra thread.) This keeps the daemon side a two-line wire-up:
construct + `open()`.

```cpp
namespace data_store {

/// Whole-process resource snapshot. All fields are int32 to match the
/// ds integer type; -1 is never used — unreadable metrics report 0.
struct StatsSample {
    std::int32_t cpu_permille = 0;   // parts-per-1000 of one host-second
    std::int32_t mem_rss_kb   = 0;
    std::int32_t fd_count     = 0;
    std::int32_t threads      = 0;
};

/// Test seam: where to read cgroup + proc from. Production defaults.
struct StatsRoots {
    std::string cgroup_root = "/sys/fs/cgroup";
    std::string proc_self   = "/proc/self";
};

/// Sink that writes the sampled key/value pairs (4 metrics + the
/// services.stats.version bump) to wherever they belong. A Client-backed
/// daemon passes `[&ds](const std::vector<KV>& kv){ ds.set(kv); }`; ds-server
/// passes a lambda over its in-process DataStore; tests capture into a
/// vector. Decoupling from Client keeps publish() unit-testable with no
/// server and no reactor, and lets ds-server (which has no Client) report.
using StatsSink = std::function<void(const std::vector<KV>&)>;

/// Periodically samples the calling process's container cgroup (CPU,
/// memory, task count) plus its own open-fd count, and publishes them via
/// the sink on an ACE reactor timer. One instance per daemon.
///
///   data_store::StatsPublisher g_stats(
///       "services.cloud.iot.cloudd",
///       [&ds](const std::vector<data_store::KV>& kv){ ds.set(kv); });
///   g_stats.open();         // spawns the ACE timer thread (every 10s)
///   ...                     // daemon does its normal recv_event loop
///   g_stats.close();        // stop timer + join thread (also in dtor)
///
class StatsPublisher : public ACE_Event_Handler {
public:
    /// @param prefix  ds key prefix, e.g. "services.cloud.iot.cloudd".
    ///                Writes "<prefix>.cpu.permille", "<prefix>.mem.rss.kb",
    ///                "<prefix>.fd.count", "<prefix>.threads", and bumps
    ///                "services.stats.version".
    /// @param sink    publishes the sampled KV batch (see StatsSink).
    /// @param roots   override cgroup/proc roots (tests only).
    StatsPublisher(std::string prefix, StatsSink sink, StatsRoots roots = {});
    ~StatsPublisher() override;

    /// Schedule the recurring timer on ACE_Reactor::instance() and spawn
    /// one ACE thread to run that singleton reactor's event loop.
    /// @param interval_sec  flush cadence (default STATS_FLUSH_SEC = 10).
    int  open(int interval_sec = STATS_FLUSH_SEC);
    /// Cancel the timer, end the reactor loop, join the thread. Idempotent.
    void close();

    /// Read cgroup + /proc/self/fd and compute CPU% since the previous
    /// sample. The FIRST call records the CPU baseline (cpu=0). Pure of the
    /// data store — unit-tested directly with fixture roots.
    StatsSample sample();

    /// ACE timer callback — calls sample() then publish().
    int handle_timeout(const ACE_Time_Value&, const void*) override;

    static constexpr int STATS_FLUSH_SEC = 10;

private:
    void publish(const StatsSample&);   // write 4 keys + bump version

    struct Impl;                        // ACE_Task thread running the
    std::unique_ptr<Impl>              m_impl;   // singleton reactor loop
    std::string                        m_prefix;
    StatsSink                          m_sink;
    StatsRoots                         m_roots;
    enum class Cg { v2, v1, none } m_cg = Cg::none;   // probed once in ctor
    unsigned long long m_last_usage_usec = 0;
    std::chrono::steady_clock::time_point m_last_t{};
    bool m_have_baseline = false;
};

}  // namespace data_store
```

The thread is encapsulated in `Impl` (an `ACE_Task<ACE_MT_SYNCH>` whose
`svc()` calls `ACE_Reactor::instance()->owner(ACE_OS::thr_self())` then
`run_reactor_event_loop()`), so the public header stays ACE-light behind the
pimpl, same discipline as `client.hpp`.

The pure parsing/math live as free functions in a `stats_detail` namespace
(declared in the header) so the unit test drives them without a reactor or a
data store:

```cpp
enum class CgVersion { v2, v1, none };   // namespace data_store

namespace stats_detail {
    CgVersion    detect_cgroup(const StatsRoots&);
    bool         read_cpu_usec(const StatsRoots&, CgVersion,
                               unsigned long long& out_usec);  // false if N/A
    std::int32_t read_mem_kb (const StatsRoots&, CgVersion);
    std::int32_t read_pids   (const StatsRoots&, CgVersion);
    std::int32_t count_fds   (const StatsRoots&);
    std::int32_t cpu_permille(unsigned long long prev_usec,
                              unsigned long long now_usec, double dt_sec);
}
```

### Sampling math

cgroup layout is **probed once** in the constructor: if
`<cgroup_root>/cgroup.controllers` exists → **v2 (unified)**; else if
`<cgroup_root>/cpu.stat`-style v1 controller dirs exist → **v1**; else
`none` (publish zeros, never throw).

- **CPU** —
  - v2: `usage_usec` line of `<cgroup_root>/cpu.stat` (cumulative µs).
  - v1: `<cgroup_root>/cpuacct/cpuacct.usage` (cumulative ns).
  Over wall interval `dt`: `percent = (Δusage_sec / dt_sec) * 100`, then
  `permille = lround(percent * 10)`, clamped to `>= 0`. (Whole-host percent:
  one fully-busy core = 1000 ‰ = 100 %.)
- **Memory** —
  - v2: `<cgroup_root>/memory.current` (bytes).
  - v1: `<cgroup_root>/memory/memory.usage_in_bytes` (bytes).
  `mem_kb = bytes / 1024`. Note this is the cgroup total (incl. page cache);
  for a single-process container it tracks the daemon's footprint closely.
- **Threads / tasks** —
  - v2: `<cgroup_root>/pids.current`.
  - v1: `<cgroup_root>/pids/pids.current`.
  This is the live task (thread) count for the whole cgroup — includes any
  child processes, which is the desired "everything in this container" number.
- **FD count** — number of entries in `<proc_self>/fd/` (a `readdir` scan,
  minus `.`/`..`). cgroups expose no fd metric, so this is the daemon's *own*
  open descriptors. Cheap, and only every ~10s.

CPU% is normalized to **whole-host** percent (a fully-busy single core =
100%; an 8-core box maxed = 800%). Document this in the UI tooltip.

## 4. Wiring per daemon

Each daemon constructs one `StatsPublisher(prefix, sink)` after its ds
connection succeeds and calls `open()`; `close()` runs on shutdown (also from
the dtor). The `UptimePublisher` in `modules/data-store/src/server/main.cpp:47`
is the precedent for the `ACE_Event_Handler` + `handle_timeout` shape.

**`run_reactor_thread` per daemon.** `open(interval, run_reactor_thread)`
decides whether `StatsPublisher` spawns its own ACE_Task thread to run the
singleton reactor, or just schedules the timer on a reactor the daemon already
pumps:

| Daemon | already runs singleton reactor? | `run_reactor_thread` |
|--------|---------------------------------|----------------------|
| iot-cloudd | no — blocks on `Client::recv_event` | **true** (spawns thread) |
| iot-httpd | yes — `handle_events` in main loop | **false** |
| lwm2m (bs/dm/client/server) | yes — `udpAdapter` ACE_Task runs it | **false** |
| ds-server | yes — `handle_events` in `main` | **false** |

ds-server has no `Client` (it *is* the store), so its sink writes via the
in-process `DataStore::set`; the others pass `[&ds](kv){ ds.set(kv); }`.

**Status:** wired for every daemon — cloud: `services.ds` (ds-server),
`services.cloud.iot.cloudd`, `services.cloud.iot.httpd`,
`services.cloud.lwm2m.bs`, `services.cloud.lwm2m.dm`; device:
`services.net.router` (net-router), `services.openvpn.client`
(openvpn-client), `services.wifi.client` (wifi-client),
`services.lwm2m.client` / `services.lwm2m.server` (device-role lwm2m). The
device daemons run blocking poll loops with no ACE reactor, so they use
`run_reactor_thread=true` (StatsPublisher spawns the ACE_Task reactor thread).
The only service with no separate publisher is `openvpn-server`: it runs
inside cloudd's container, so its usage is folded into cloudd's cgroup totals
(its `services.cloud.openvpn.server.*` keys stay 0).

| Daemon | main.cpp | Prefix |
|--------|----------|--------|
| ds-server | `modules/data-store/src/server/main.cpp` | `services.ds` |
| iot-cloudd | `apps/cloud/server/src/main.cpp` | `services.cloud.iot.cloudd` |
| iot-httpd | `modules/http-server/src/main.cpp` | `services.cloud.iot.httpd` (device: `services` n/a — see §7) |
| lwm2m (bs/dm/client/server) | `apps/src/main.cpp` | role-dependent (`services.cloud.lwm2m.bs`, `…dm`, `services.lwm2m.client`, `services.lwm2m.server`) |
| openvpn-client | `modules/openvpn/client/src/main.cpp` | `services.openvpn.client` |
| net-router | `modules/net/router/src/main.cpp` | `services.net.router` |
| wifi-client | `modules/wan/wifi/client/src/main.cpp` | `services.wifi.client` |

The lwm2m binary already picks its `log.lwm2m.*` key by role (`lwm2m-instance=bs|dm`,
client vs server) — the stats prefix is selected by the same switch.

**openvpn(8) / wpa_supplicant** — these third-party children run *inside the
same container* as their managing daemon (openvpn-client / wifi-client), so
the cgroup CPU/memory/pids totals already include them. No extra
`StatsPublisher` instance needed. (FD count is the managing daemon's own
descriptors only — a known, acceptable limitation, since the child's fds
live in its own `/proc/<child>/fd`.)

### Cadence

One fixed ACE reactor-timer interval, **`STATS_FLUSH_SEC = 10`**
(`StatsPublisher::STATS_FLUSH_SEC`, scheduled on `ACE_Reactor::instance()` via
`schedule_timer` with a 10 s repeat `ACE_Time_Value`). Rationale:
- cgroup + one `readdir` is microseconds of work — frequency is not a cost
  concern; the limit is signal usefulness and ds write churn.
- 10 s gives a stable CPU% average (CPU is a *delta over the interval*, so a
  shorter window is noisier; a longer one lags). It comfortably feeds the
  cloud-UI Services refresh (≈5 s long-poll today — see `apps/cloud/CLAUDE.md`).
- The bump key `services.stats.version` moves at most once per flush, so the
  UI long-poll wakes ~every 10 s regardless of how many daemons there are.

Optional (deferred): make it runtime-tunable via a ds key
`services.stats.interval.sec` (default 10), hot-reloaded on watch — same
pattern as `log.level`. Not needed for v1; the constant is enough.

## 5. HTTP / API

No new endpoint. The cloud UI already reads ds keys directly through
`POST /api/v1/db/get` and long-polls via `GET /api/v1/db/get?key=…`. The
Services page simply adds the new keys to the batch it already fetches and
switches its long-poll target to `services.stats.version`.

(Optional, deferred: enrich the `/api/v1/status` `services{}` block in
`modules/http-server/src/handler.cpp` with `cpu_permille` / `mem_kb` so the
Dashboard can show aggregates. Not required for this feature.)

## 6. Cloud UI — Services page

File: `apps/cloud/ui/src/app/services/services-list/services-list.component.ts`

Convert the `.svc-grid` card layout to a Clarity `clr-datagrid`, reusing the
structure of `apps/cloud/ui/src/app/endpoint-list/endpoint-list.component.ts`.

Columns:

| Column | Source |
|--------|--------|
| Service | `s.label` (code-formatted) |
| State | `<app-status-badge [state]="s.state">` (reused) |
| CPU % | `s.cpu_permille / 10` → e.g. `12.3 %` |
| Memory | `s.mem_kb` → humanized (`fmtKb`: KB / MB) |
| FDs | `s.fd_count` |
| Threads | `s.threads` |
| Enabled | existing enable checkbox (admin-gated) |
| Actions | existing Restart button (admin-gated) |

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Service Management                                                             │
│  clr-datagrid:                                                                 │
│  ┌────────────┬─────────┬───────┬─────────┬─────┬─────────┬─────────┬────────┐ │
│  │ Service    │ State   │ CPU % │ Memory  │ FDs │ Threads │ Enabled │ Action │ │
│  │ ds-server  │ running │  0.4  │ 4.1 MB  │  12 │    4    │ on(—)   │ on     │ │
│  │ iot-cloudd │ running │  1.2  │ 12.8 MB │  23 │    7    │ ☑       │[Restart]│ │
│  │ iot-httpd  │ running │  0.8  │  9.0 MB │  18 │    6    │ ☑       │[Restart]│ │
│  └────────────┴─────────┴───────┴─────────┴─────┴─────────┴─────────┴────────┘ │
└──────────────────────────────────────────────────────────────────────────────┘
```

Data plumbing (extends existing `fetchAll` / `applyState`):

- `SvcRow` gains `cpu_permille?: number; mem_kb?: number; fd_count?: number; threads?: number;`.
- `fetchAll()` adds the four `services.<name>.{cpu.permille,mem.rss.kb,fd.count,threads}`
  keys for every row to the `dbGet` batch.
- `applyState()` reads the four numbers into each row.
- `scheduleLongPoll()` replaces the round-robin over `.state` keys with a
  single long-poll on `services.stats.version` (the bump key already moves on
  every flush, ~10s, which also reflects state changes if we bump on state
  writes — otherwise keep one `.state` poll in parallel). **Decision for
  TDD:** keep the existing per-state round-robin AND add the version poll, or
  consolidate. Simplest correct: long-poll `services.stats.version`, and on
  each tick re-`fetchAll()` (which already re-reads state + enable too).

No backend change needed for the UI; everything is ds-key driven.

## 7. Device UI note

`iot-ui/src/app/services/services-list/services-list.component.ts` is the
device-side twin. The same column additions apply verbatim if/when desired —
the device daemons (ds, lwm2m client, openvpn-client, net-router,
wifi-client) publish the same `services.<name>.*` keys. **v1 targets the
cloud UI only** (per request); the device UI change is a trivial follow-up.

## 8. Reuse summary

| Existing thing | Reused as |
|----------------|-----------|
| `LogBuffer` (lib pattern) | shape for `StatsPublisher` |
| `UptimePublisher` reactor timer | shape for the flush timer |
| `read_meminfo()` `/proc` reader | small-file `/proc`/`/sys` read idiom |
| `log.version` single-key long-poll | `services.stats.version` |
| `services.ds.uptime.sec` typed key | shape for `cpu.permille` / `mem.rss.kb` |
| Endpoints `clr-datagrid` | Services table |
| `app-status-badge` | State column |

## 9. TDD plan (next step)

1. **StatsPublisher unit test** — point `Roots` at fixture dirs: a fake
   cgroup root (`cgroup.controllers` + `cpu.stat` with `usage_usec`,
   `memory.current`, `pids.current`) and a fake `proc_self` with an `fd/`
   directory. Assert CPU permille from a known `usage_usec` delta over a known
   interval, `mem.rss.kb` from `memory.current`, `threads` from
   `pids.current`, `fd.count` from the `fd/` entry count; assert first-call
   baseline behavior (cpu = 0) and graceful zeros when files are absent
   (`CgVersion::none`). Add a v1-layout fixture to cover the fallback.
2. **Schema test** — load `services.lua`, assert the four new keys per service
   exist with type=integer, min=0, access=Viewer.
3. **Integration** — start ds-server + a daemon with StatsPublisher, wait one
   flush, `ds-cli get services.<name>.{cpu.permille,mem.rss.kb,fd.count,threads}`
   and `services.stats.version` are populated and version increments on the
   next flush.
4. **Cloud UI component test** — `ServicesListComponent` renders the datagrid,
   maps the four numeric keys into the CPU%/Memory/FDs/Threads columns,
   humanizes KB, and long-polls `services.stats.version`.
