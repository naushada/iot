# L16 Plan — services.* enable plane

> Forward-looking phase plan. Same shape as L11–L15. Adds a
> per-daemon `services.<name>.enable=true/false` control plane on
> top of the existing ds-server schema model, so operators can
> start and stop the worker subprocesses (`openvpn(8)`,
> `wpa_supplicant`, `udhcpc`, `nft …`) without `systemctl` and
> without root. systemd-level stop stays the escape hatch.
>
> Runs independently of L15 except for D6 (wifi-client wiring,
> which needs `modules/wan/wifi/client/` to exist). D1–D4 + D7
> ship without touching L15; D5 + D6 land after their respective
> daemons are in.
>
> **Status (2026-06-01):** plan only.

---

## 0. Goal

A central `services.lua` schema plus a tiny shared `services::Gate`
helper that every daemon's Supervisor consults alongside its
existing gates. End state:

```
operator                                                  ds-server
   │                                                          │
   │  ds-cli svc disable openvpn.client                       │
   ├─────────────────────────────────────────────────────────▶│
   │                                                          │
   │                                          changed event   │
   │   openvpn-client Supervisor◀─────────────────────────────┤
   │   - Gate flips closed                                    │
   │   - Active session: SIGTERM + reap openvpn(8)            │
   │   - publish services.openvpn.client.state="disabled"     │
   │   - WAN gate ignored while disabled                      │
   │                                                          │
   │  ds-cli svc enable openvpn.client                        │
   ├─────────────────────────────────────────────────────────▶│
   │                                                          │
   │   Supervisor◀────────────────────────────────────────────┤
   │   - Gate flips open; WAN gate evaluated as usual         │
   │   - state="starting" then "running" once child up        │
   │                                                          │
```

Concretely, this phase delivers:

1. `modules/data-store/schemas/services.lua` declaring
   `services.<name>.enable` (boolean, default true) and
   `services.<name>.state` (string enum) for every gateable daemon.
2. A `services::Gate` helper (in `modules/data-store/inc/` or a
   new `modules/services/` if a sibling lib feels lighter) that
   any daemon's Supervisor can compose with.
3. Wiring per-daemon: ds-server (state only), net-router,
   openvpn-client, lwm2m-client, lwm2m-server, wifi-client.
4. `ds-cli svc list / enable / disable / status` convenience
   subcommands.

### Non-goals (first-cut scope)

- **systemd-unit control.** No `systemctl start/stop` from the
  daemons. Operators use `systemctl stop iot-<name>` directly if
  they need to stop the whole unit. The services.* plane is for
  *worker* control only, where the daemon stays running and
  observable.
- **Dependency ordering.** `services.<a>.enable` doesn't imply
  anything about `services.<b>.enable`. If an operator disables
  net-router while openvpn-client is up, the WAN gate will close
  and openvpn-client will park as-if WAN-down — that's the
  existing L13 behavior, not new policy.
- **Boot-time persistence policy.** The values live in
  `data_store.lua` like any other key — restart-persistent by
  default. No "ephemeral disable" or "disable until next boot"
  mode in v1.
- **RBAC.** Filesystem DAC on the ds-server socket is the only
  access control, identical to the existing get/set surface.
  Per-key ACL is FUP.
- **Worker-internal granularity.** `services.wifi.client.enable`
  toggles the whole wifi worker chain (wpa_supplicant + udhcpc);
  there is no `services.wifi.client.dhcp.enable` sub-key in v1.
- **ds-server self-disable.** ds-server publishes
  `services.ds.state="running"` for uniformity but rejects
  `set services.ds.enable` — disabling the substrate via the
  substrate is a trap.

---

## 1. Risk register

| ID  | Risk                                                                                                          | Mitigation                                                                                                                                                                                                       |
|-----|---------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| R1  | Operator disables a service then can't get state back because the daemon is "off"                              | Daemons stay alive on `enable=false`; the DsBridge keeps publishing `services.<name>.state` so observability survives. Only the worker subprocess is reaped.                                                     |
| R2  | A daemon is gated by *both* `services.<self>.enable` AND a domain gate (WAN for openvpn) — semantics unclear   | Define gate composition explicitly: enable=false dominates. WAN-down + enable=true → `gate.reason="wan_down"`. enable=false → `gate.reason="disabled"`, WAN snapshot ignored.                                    |
| R3  | Stray `services.<name>.enable=false` in `data_store.lua` after a crash → daemon refuses to come up post-reboot | Document the recovery path in DEPLOY.md (`ds-cli set services.<n>.enable true`). Boot-time default is true; absent key behaves as enabled.                                                                       |
| R4  | `services.ds.enable` accidentally honored → ds-server self-shuts; everyone disconnects                         | ds-server **rejects set on services.ds.enable** with `SchemaRejected("services.ds.enable is read-only on ds-server")`. Schema entry omits the boolean type and marks it `readonly=true` (new schema attribute). |
| R5  | Race: `set services.X.enable false` lands before the daemon's DsBridge has primed                              | Same race the WAN gate already has. Supervisor evaluates the gate on every event tick (250 ms), so a missed event still resolves within one tick of the snapshot finally landing.                                |
| R6  | lwm2m-client has no worker subprocess — what does "disabled" mean?                                             | Define lwm2m-client "disabled" = skip Register, drop active observations, keep the CoAP socket listening. Same for lwm2m-server (stop accepting registrations, keep socket). Detailed in D5.                     |
| R7  | net-router "disabled" while a route is installed → stale route never gets cleaned up                           | On `enable=false`, net-router runs its full teardown path (flush nft tables it owns, clear `net.iface.active`). Same path SIGTERM already takes today.                                                          |
| R8  | `ds-cli svc list` parses every `services.*` row but the schema has unrelated rows interleaved                  | The `services.lua` schema is the only legitimate source of `services.*` keys; ds-cli queries the schema (via a new `schema-dump` op, D7) rather than scanning the key space. Keeps the surface tight.            |
| R9  | Worker subprocess in a stuck wait (e.g., openvpn DNS resolve hung) doesn't honor SIGTERM in 5 s                | The existing 5 s SIGTERM→SIGKILL escalation in `OpenVpnProcess::~Process` handles it. Same logic copies into wifi-client (REQ-WIFI per L15 NFR-WIFI-006).                                                       |
| R10 | enable→false→true rapid bounce floods the journal with spawn/reap log lines                                     | Coalesce: ignore enable transitions whose values haven't actually changed (data-store already de-dupes identical sets, but the boolean → boolean parser must too). Document a 1 s debounce as a possible v2.    |
| R11 | Existing daemons in the field don't have `services.*.enable` in their data store → behavior changes on upgrade  | Schema default = `true`. A pre-existing data_store.lua without the key resolves to "enabled" on first read. No upgrade surprise.                                                                                  |

---

## 2. D-items

### D1 — plan + central schema + shared Gate

**Scope.** This file, `modules/data-store/schemas/services.lua`,
and a small shared header/source.

#### `services.lua` shape

```lua
return {
  namespace = "services",
  keys = {
    -- ds-server: read-only state surface; enable is rejected.
    ["services.ds.state"]                  = { type = "string", default = "running" },
    ["services.ds.uptime.sec"]             = { type = "integer", min = 0, default = 0 },

    -- net-router
    ["services.net.router.enable"]         = { type = "boolean", default = true },
    ["services.net.router.state"]          = { type = "string",  default = "running" },

    -- openvpn-client
    ["services.openvpn.client.enable"]     = { type = "boolean", default = true },
    ["services.openvpn.client.state"]      = { type = "string",  default = "running" },

    -- lwm2m-client / lwm2m-server
    ["services.lwm2m.client.enable"]       = { type = "boolean", default = true },
    ["services.lwm2m.client.state"]        = { type = "string",  default = "running" },
    ["services.lwm2m.server.enable"]       = { type = "boolean", default = true },
    ["services.lwm2m.server.state"]        = { type = "string",  default = "running" },

    -- wifi-client (lands when L15/D6 ships)
    ["services.wifi.client.enable"]        = { type = "boolean", default = true },
    ["services.wifi.client.state"]         = { type = "string",  default = "running" },
  },
}
```

`services.ds.enable` is **deliberately absent** — set on a missing
key against a schema that has the namespace defined is rejected
under the existing ds-server flow (unknown-keys-passthrough does
not apply when the namespace owner has declared its surface).
Tested in D2.

#### `services.<name>.state` enum

```
"running"   - worker subprocess up (or main loop active, for lwm2m)
"disabled"  - operator-gated; worker not running
"starting"  - enable=true seen, worker spawn in progress
"stopping"  - enable=false seen, worker reap in progress
"exited"    - worker crashed/exited; daemon still alive, will retry
"conflict"  - precondition failed (e.g., NM owns iface for wifi)
```

The first four are universal; "exited" and "conflict" are
domain-specific and may not show up for every daemon. lwm2m
mostly cycles through running/disabled.

#### Shared `services::Gate`

Lift the openvpn-client pattern into a tiny shared helper.
Header lives at `modules/data-store/inc/data_store/service_gate.hpp`
(co-located with the client lib so any daemon already linking
libdatastore_client gets it for free).

```cpp
namespace data_store {

/// Service-enable gate, shared across all iot daemons.
/// Wraps the (DsBridge → on_change → Supervisor) pattern that
/// openvpn-client's WAN gate already uses. Composes with any
/// other gates the Supervisor has: closed if ANY gate is closed.
class ServiceGate {
public:
    explicit ServiceGate(Client& ds, std::string key); // "services.openvpn.client.enable"

    /// Snapshot the latest value (true on absent / default).
    bool enabled() const;

    /// Block until enabled() changes, or shutdown() is called.
    /// Returns the new value; nullopt on shutdown.
    std::optional<bool> wait();

    /// Wake any wait()ing thread; subsequent enabled() reflects
    /// the latest snapshot.
    void shutdown();

    /// Publish state transitions to "services.<name>.state".
    /// Best-effort; failures logged via ACE_ERROR.
    void publish_state(const std::string& s);

private:
    // … listener thread, mutex, cv, m_value, m_shutdown
};

} // namespace data_store
```

**Tests.** Unit tests cover absent→default-true, set→false→true
transitions, multi-waiter wakeup, shutdown drains waiters.

### D2 — ds-server self-publishes + set-rejection

**Scope.** ds-server publishes its own row and refuses to honor a
set on `services.ds.enable`.

1. On startup, ds-server's main loop writes
   `services.ds.state="running"` once it's accepted its first
   connection. (No "starting" → "running" transition; the only
   transition ds itself owns is exit, and at that point nobody
   is reading.)
2. A periodic 60 s tick updates `services.ds.uptime.sec`. Same
   `ACE_Reactor` timer the existing `iot.binding`-ping uses.
3. `services.ds.enable` set rejection: SchemaRegistry gains a
   `readonly` attribute (boolean, default false). The schema
   loader maps a *missing* boolean type + `readonly=true` to
   "always reject set". ds-cli sees `SchemaRejected("readonly")`.

Alternative (simpler, picked at D2): don't add a `readonly`
attribute; just leave `services.ds.enable` out of the schema and
let the namespace-claimed-but-key-not-declared path reject it.
SchemaRegistry already supports this. (Note in §5 if we take
this path.)

**Tests.** `Schema.services_ds_enable_rejected`,
`Schema.services_ds_state_published_on_boot`.

### D3 — net-router enable gate

**Scope.** `modules/net/router/src/supervisor.cpp` (or
`main_impl.cpp` if net-router doesn't yet have a Supervisor —
the iface_monitor loop is what gets gated).

Pre-disable (`enable=true`): unchanged behavior. iface_monitor
runs, publishes `net.iface.active`.

On `enable=false`:
- Pause iface_monitor.
- Run the full teardown: `nft delete table iot-fwd` (or whatever
  net-router owns), clear `net.iface.active=""`, publish
  `services.net.router.state="disabled"`.
- Stay parked in a `cv.wait` on the ServiceGate.

On `enable=true`:
- Re-prime the iface scan, re-create nft state, publish
  `services.net.router.state="running"`.

**Tests.** `net-router/test/supervisor_test.cpp::Services_disable_clears_active_iface`,
`Services_reenable_restores_publishing`.

### D4 — openvpn-client enable gate (composes with WAN)

**Scope.** `modules/openvpn/client/src/supervisor.{hpp,cpp}` —
add a second gate.

Today's Supervisor has one gate (`m_gate` from WAN). Add
`m_svc_gate`. The Supervisor's wait/serve loop becomes:

```
loop:
  if !svc_gate.enabled():
    state = "disabled"; gate.reason = "disabled"
    svc_gate.wait()                       # block on enable transition
    continue
  if !wan_target:
    state = "running" /* idle */; gate.reason = "wan_down"
    wait_for_event()                      # WAN OR svc
    continue
  serve_one_session(wan_target)           # existing path
```

Composition rule: **disable dominates WAN.**
`gate.reason="disabled"` when both are closed, never "wan_down".

**Tests.**
- `supervisor_test.cpp::Services_disable_kills_active_session` —
  during `running`, flip enable=false, prove the openvpn child is
  reaped + state="disabled" within one event-loop tick.
- `Services_reenable_after_wan_returns_starts_session` — enable
  false, then WAN comes/goes, then enable=true — confirm exactly
  one spawn at the end.

### D5 — lwm2m-client + lwm2m-server enable gate

**Scope.** `apps/src/lwm2m_main.cpp` (or wherever the role
dispatch sits) plus a gate-aware Register loop.

lwm2m has no spawnable worker — it *is* the worker. "Disabled"
means:

- **lwm2m-client**: skip Register at startup if disabled. If
  disabled while registered, send Deregister + clear active
  observations; keep the CoAP socket listening so we can be
  re-enabled without restart.
- **lwm2m-server**: stop accepting Register requests
  (HTTP 503-ish at the LwM2M layer; precise code at D5). Drop
  the active-registrations map but keep the listening socket.

This is the most invasive piece — touches the registration FSM.
Risk-budget it: if D5 grows past one PR, split into D5a (client)
and D5b (server).

**Tests.** `apps/test/lwm2m_lifecycle_test.cpp::Services_disable_deregisters_and_stops_polling`,
`Services_reenable_reregisters_from_scratch`.

### D6 — wifi-client enable gate

**Scope.** Wires `services.wifi.client.enable` into the
wifi-client Supervisor designed in L15/D6. Composes with the
NetworkManager-conflict gate from L15/REQ-WIFI-022.

Depends on L15 D6 landing. If L16/D6 runs before L15/D6, the
plan slips D6 to "after L15 closes" — D1..D5 + D7 can ship
independently.

**Tests.** `wifi-client/test/supervisor_test.cpp::Services_disable_reaps_wpa_and_dhcp`.

### D7 — `ds-cli svc` subcommand

**Scope.** `modules/data-store/src/cli/svc.{hpp,cpp}` plus a new
`schema-dump` op on the ds-server protocol so `ds-cli svc list`
can enumerate the services row set without hardcoding it.

```
$ ds-cli svc list
NAME             ENABLE  STATE      UPTIME
ds               n/a     running    1h12m
net.router       true    running    1h12m
openvpn.client   true    running    47m
lwm2m.client     true    running    1h11m
lwm2m.server     true    running    1h11m
wifi.client      false   disabled   1h12m

$ ds-cli svc disable openvpn.client
ok

$ ds-cli svc status openvpn.client
services.openvpn.client.enable = true
services.openvpn.client.state  = stopping
```

`enable` `disable` `status` are thin wrappers over the existing
`set` / `get` primitives. `list` needs the new `schema-dump` op
so we don't hand-maintain a list in the CLI.

**Tests.** `ds-cli` smoke under `log/L16/run-svc-smoke.sh`:
covers list, enable, disable, status round-trip + wire transcript
into `log/L16/svc-smoke.txt`.

### D8 — DEPLOY.md + smoke harness

**Scope.**

- `DEPLOY.md`: add a `services.* control plane` section showing
  `ds-cli svc` examples and the systemctl-vs-ds-cli distinction
  ("systemctl owns the daemon; ds-cli owns the worker").
- `log/L16/smoke.sh`: container-side e2e. Brings up
  ds + net-router + openvpn-client via compose (reusing L14's
  compose path). Disables openvpn-client via ds-cli, asserts the
  openvpn process is gone (no `pgrep openvpn` match). Re-enables,
  asserts it comes back. Same for net-router. lwm2m + wifi-client
  smoke deferred to their respective integration phases.

---

## 3. Acceptance for L16

L16 closes when:

- `bash log/L16/smoke.sh` exits 0 on a clean machine with podman.
- `services.lua` is enforced by ds-server (set on
  `services.ds.enable` → `SchemaRejected`; set boolean on
  `services.<name>.enable` for any other name → accepted).
- Eight D-PRs merged (D1..D8), or D6 explicitly carried to a
  follow-up if L15 D6 hasn't landed yet (the only cross-phase
  dependency).
- Each gated daemon's `docs/design.md` documents the new gate +
  the composition rule (services dominates WAN, services
  dominates conflict).
- DEPLOY.md shows the `ds-cli svc` commands alongside the
  existing systemctl invocations.

---

## 4. After L16

Candidate next phases (not committed):

- **L17a — services dependency graph.** `services.<a>.depends_on`
  schema entries so disabling net-router auto-publishes a
  `services.openvpn.client.gate.reason="dep_down"` instead of
  silently relying on the WAN gate to do the same thing.
- **L17b — ephemeral disable.** `ds-cli svc disable --until-boot`
  → set into an in-memory overlay that doesn't persist to
  `data_store.lua`. Needs a new ds-server "volatile" namespace.
- **L17c — per-key ACL.** Restrict who can set `services.*.enable`
  vs. who can set `wifi.networks`. Same socket DAC isn't enough
  once we have third-party operators on the box.
- **L17d — rate-limit / chaos coverage.** Disable/enable churn
  cap + a chaos-test harness that flips every services.*.enable
  randomly and asserts the daemons stay stable. Pairs with the
  L16f candidate from the L15 plan.

Pick one when L16 is in.
