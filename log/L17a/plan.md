# L17a Plan — services dependency graph

> Forward-looking phase plan. Same shape as L11–L16. Adds a
> `services.<name>.depends_on` array to the services schema so
> operators get an explicit `gate.reason="dep_down:<name>"`
> instead of silently falling through to "wan_down" when a
> dependency is disabled.
>
> Runs independently of all other phases. D1–D4 ship together
> and touch only `services.lua` + the per-daemon Supervisor
> loops — no new IPC, no new daemons, no polling.

---

## 0. Goal

Today (post-L16), if an operator runs `ds-cli svc disable net.router`,
the downstream effect is:

```
operator disables net.router
  → net.iface.active = ""
  → openvpn-client WAN gate closes
  → vpn.gate.reason = "wan_down"   ← wrong: WAN is fine, net-router is off
```

The operator sees "wan_down" and checks cables. The real cause —
"net.router is disabled" — is invisible. L17a fixes this by
making the dependency graph explicit:

```
operator disables net.router
  → services.net.router.state = "disabled"
  → openvpn-client sees dep "net.router" is disabled
  → vpn.gate.reason = "dep_down:net.router"   ← correct
```

Concretely, this phase delivers:

1. `services.lua` gains an optional `depends_on` array per service.
2. A shared `services::DepWatch` helper that any daemon's Supervisor
   can compose alongside its existing `ServiceGate` — subscribes to
   `services.<dep>.state` for each declared dependency and signals
   the Supervisor when any dep goes disabled.
3. Wiring per-daemon: net-router (no deps; leaf), openvpn-client
   (depends on net.router), wifi-client (depends on net.router),
   lwm2m-client (depends on net.router), lwm2m-server (depends on
   net.router).
4. `ds-cli svc list` shows a `DEPENDS` column so operators can
   inspect the graph without reading the schema.

### Dependency graph (v1)

```
net.router
  ↑ (depends on)
  ├── openvpn.client
  ├── wifi.client
  ├── lwm2m.client
  └── lwm2m.server
```

Every daemon except net-router depends on net-router. This is the
only edge in v1. Future phases (L17a-follow-up) can add:
- `lwm2m.client` depends on `openvpn.client` (if LwM2M must go
  through the VPN tunnel)
- `wifi.client` depends on `openvpn.client` (if wifi auth goes
  through VPN)

But these are deployment-specific; v1 ships the single universal
edge (everything needs forwarding).

### Runtime flow (push-based, reusing existing ServiceGate mechanism)

End-to-end on `ds-cli set services.net.router.enable false`:

1. **ds-server** persists the value and fans out `changed` to every
   daemon that watches `services.net.router.enable` — that's the
   net-router daemon itself (its own ServiceGate) plus nothing else
   (no other daemon watches that key today).

2. **net-router Supervisor** wakes, transitions to disabled, and
   publishes `services.net.router.state="disabled"`.

3. **Every dependent daemon** watches `services.net.router.state`
   via the new `DepWatch` helper. ds-server fans out `changed` for
   that key to all of them.

4. **Dependent Supervisor** wakes, sets `gate.reason="dep_down:net.router"`,
   and reaps/pauses its worker. This is the exact same code path
   L16 already has for `enable=false` — the composition rule just
   adds another clause.

```
  operator                  ds-server                    net-router        openvpn-client
     │                          │                           │                    │
     │  set net.router.enable  │                           │                    │
     │  false                   │                           │                    │
     ├──────────────────────────▶                           │                    │
     │                          │                           │                    │
     │                          │  changed: net.router      │                    │
     │                          │  enable = false           │                    │
     │                          ├──────────────────────────▶│                    │
     │                          │                           │                    │
     │                          │                           │  state="disabled"  │
     │                          │                           │─────┐              │
     │                          │                           │     │              │
     │                          │  changed: net.router      │◀────┘              │
     │                          │  state = "disabled"       │                    │
     │                          ├───────────────────────────┼───────────────────▶│
     │                          │                           │                    │
     │                          │                           │     DepWatch wakes │
     │                          │                           │     reason=dep_down│
     │                          │                           │     reap openvpn   │
     │                          │                           │                    │
```

**Cost.** One extra `register` call per dependency at daemon startup
(already amortized into the existing DsBridge prime), one extra
`changed` line per dependency transition (cheap: ~100 bytes of JSON
over a unix socket), one extra `enabled()` check in the Supervisor
loop (a mutex-guarded bool read). No new threads, no polling.

---

## 1. Risk register

| ID  | Risk                                                                                                          | Mitigation                                                                                                                                                                                                       |
|-----|---------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| R1  | Circular dependency (A depends on B depends on A) creates deadlock                                            | Reject at schema-load time: ds-server walks the graph on boot (or on schema reload) and refuses to start if a cycle is detected. `depends_on` is a DAG.                                                          |
| R2  | Operator disables net-router, then immediately re-enables — transient "dep_down" flicker                      | The existing enable-coalescing (REQ-DS-006 in ds-server) suppresses unchanged-value sets. A disable→enable round-trip through net-router takes one tick to publish "disabled" and one tick to publish "running". The dependent's DepWatch sees the "running" transition within 250ms. Flicker is bounded to one event-loop tick. |
| R3  | Dependency key not in schema → DepWatch silently fails                                                        | DepWatch validates at construction: every name in `depends_on` must resolve to a declared `services.<name>.state` key in the schema. Missing dep → daemon refuses to start with a clear ACE_ERROR message.        |
| R4  | Transitive deps: A depends on B depends on C; C disabled → B disabled → A sees "dep_down:B" — operator double-checks B, finds B disabled by C, has to chase | This is the correct behavior — the operator disabled C and the cascade is visible at every hop. `ds-cli svc status` shows the full chain. L17a-follow-up can add `dep_chain` to the reason string.                |
| R5  | Upgrade: existing deployments don't have `depends_on` in services.lua                                         | `depends_on` defaults to empty array. A pre-L17a `services.lua` loads fine. Daemons with no declared deps behave identically to L16.                                                                             |
| R6  | DepWatch subscription races with ds-server restart → missed transition                                        | Same mitigation as L16 ServiceGate: the 250ms recv timeout in each daemon's Supervisor re-evaluates every gate on each tick. DepWatch + ServiceGate compose under the same cv/notify mechanism.                   |

---

## 2. D-items

### D1 — plan + schema + shared DepWatch

**Scope.** This file, `modules/data-store/schemas/services.lua`
(adds `depends_on` arrays), and a small shared header/source.

#### `services.lua` diff

```lua
-- Existing keys gain an optional depends_on declaration:
["services.net.router.enable"]  = { type = "boolean", default = true,
                                     depends_on = {} },
["services.openvpn.client.enable"] = { type = "boolean", default = true,
                                        depends_on = {"net.router"} },
["services.wifi.client.enable"]    = { type = "boolean", default = true,
                                        depends_on = {"net.router"} },
["services.lwm2m.client.enable"]   = { type = "boolean", default = true,
                                        depends_on = {"net.router"} },
["services.lwm2m.server.enable"]   = { type = "boolean", default = true,
                                        depends_on = {"net.router"} },
```

`net.router` has an empty deps array (it's the leaf). `ds` has no
`enable` key at all (read-only). The `depends_on` is schema
metadata — it doesn't create new keys, just annotates existing ones.

#### Schema validation (ds-server)

- At schema load, ds-server walks every key's `depends_on` and
  verifies: (a) the referenced name is a declared `services.<n>.state`
  key, (b) no cycles exist in the full graph. Violations → server
  refuses to start with a message to stderr.

#### Shared `services::DepWatch`

Lives alongside `ServiceGate` in `modules/data-store/inc/data_store/service_gate.hpp`
and `src/client/service_gate.cpp`. One `DepWatch` per daemon with
dependencies.

```cpp
namespace data_store {

/// Dependency-state watch, shared across all iot daemons (L17a/D1).
///
/// Composes with ServiceGate: ServiceGate owns the enable/disable
/// fence; DepWatch owns the "are my dependencies healthy?" fence.
/// The Supervisor evaluates both in its wake loop — if ANY dep is
/// disabled, gate.reason="dep_down:<name>" regardless of the
/// ServiceGate's own enabled() value.
///
/// Threading: same pattern as ServiceGate. The data_store::Client's
/// internal listener thread updates snapshots under a mutex; the
/// Supervisor's main thread calls healthy() and wait().

class DepWatch {
public:
    /// `client` MUST be a connected data_store::Client. `deps` is
    /// the list of bare daemon names this daemon depends on
    /// (e.g., {"net.router"}). For each dep, DepWatch subscribes
    /// to `services.<dep>.state`.
    DepWatch(Client& client, std::vector<std::string> deps);

    /// True when every dependency is in a healthy state (state is
    /// "running" or "starting"). False if any dep is "disabled",
    /// "stopping", or "exited". "conflict" counts as unhealthy.
    bool healthy() const;

    /// Returns the name of the first unhealthy dependency, or
    /// empty string when healthy.
    std::string unhealthy_dep() const;

    /// Block until a dependency state changes OR shutdown() is
    /// called. Returns true on state change, false on shutdown.
    bool wait();

    /// Wake any blocked thread. Idempotent.
    void shutdown();

    /// Number of declared dependencies.
    std::size_t count() const { return m_deps.size(); }

private:
    Client&                       m_client;
    std::vector<std::string>      m_deps;        // bare names: "net.router"
    std::vector<std::string>      m_state_keys;  // "services.net.router.state"

    mutable std::mutex            m_mtx;
    std::condition_variable       m_cv;
    std::vector<bool>             m_healthy;     // one per dep
    bool                          m_shutdown = false;
};

} // namespace data_store
```

**Tests.** `modules/data-store/test/dep_watch_test.cpp`:
- `DEP_REQ_001_empty_deps_always_healthy` — DepWatch with no deps
  returns healthy()==true immediately.
- `DEP_REQ_002_dep_disabled_makes_unhealthy` — set dep state to
  "disabled", verify unhealthy_dep() returns its name.
- `DEP_REQ_003_dep_starting_counts_as_healthy` — "starting" is a
  transient state but the dep is not yet serving; we treat it as
  healthy (the dependent waits for it to reach "running" before
  spawning its own worker, but the gate reason stays "dep_down"
  only for truly disabled deps).
- `DEP_REQ_004_wait_returns_on_state_change` — block in wait(),
  set dep state → waiter wakes.
- `DEP_REQ_005_multi_dep_first_unhealthy_wins` — three deps, disable
  the second → unhealthy_dep() returns the second's name.

### D2 — net-router: publish dep-aware state (leaf)

**Scope.** `modules/net/router/src/daemon.cpp` — minimal change.

net-router is the leaf of the dependency graph (nothing depends on
it that it doesn't declare). But it does publish
`services.net.router.state`, which is what every other daemon
watches. No DepWatch is needed in net-router itself (it has zero
dependencies). The one change: when net-router transitions to
"disabled", it should briefly set its state to "stopping" (it
already does this in L16/D3) and then "disabled" — no new code,
just verifying the existing state transitions satisfy the
dependency contract.

**Tests.** Existing `SVC_REQ_NR_001` and `SVC_REQ_NR_002` in
`supervisor_test.cpp` already cover the disable/re-enable state
transitions. No new tests needed.

### D3 — openvpn-client: gate.reason="dep_down:net.router"

**Scope.** `modules/openvpn/client/src/supervisor.{hpp,cpp}` —
adds `DepWatch` alongside the existing `ServiceGate`.

The Supervisor's composition rule (L16/D4) currently says: enable
dominates WAN. L17a extends it: **dep-down dominates enable**.
If net-router is disabled, the VPN cannot function regardless of
the WAN state or the enable flag.

Updated loop:

```
loop:
  if !dep_watch.healthy():
    state = "disabled"; gate.reason = "dep_down:" + dep_watch.unhealthy_dep()
    dep_watch.wait()                        # block until deps recover
    continue
  if !svc_gate.enabled():
    state = "disabled"; gate.reason = "disabled"
    svc_gate.wait()                         # block on enable transition
    continue
  if !wan_target:
    state = "running" /* idle */; gate.reason = "wan_down"
    wait_for_event()                        # WAN OR svc OR dep
    continue
  serve_one_session(wan_target)
```

Composition priority: **dep_down > disabled > wan_down**.

**Tests.** `modules/openvpn/client/test/gate_test.cpp` additions:
- `DepReason_net_router_disabled_sets_dep_down` — dep unhealthy
  → gate.reason contains "dep_down:net.router".
- `DepReason_deps_healthy_service_disabled_still_disabled` — deps
  healthy but enable=false → reason="disabled" (not dep_down).
- `DepReason_deps_recover_resumes_wan_evaluation` — dep transitions
  from disabled→running → Supervisor resumes normal WAN gating.

### D4 — wifi-client + lwm2m-client + lwm2m-server wiring

**Scope.** `modules/wan/wifi/client/src/supervisor.{hpp,cpp}`,
`apps/src/main.cpp` — add `DepWatch` alongside existing
`ServiceGate` in each daemon.

Each daemon declares `depends_on = {"net.router"}` and constructs
a `DepWatch` at startup (reusing the same `data_store::Client*`
the `ServiceGate` already uses). The composition rule is the same
as D3: dep_down dominates enable, which dominates NM-conflict (for
wifi) or the LwM2M registration FSM.

**lwm2m-client**: When dep_down, skip Register (same behavior as
`enable=false` in L16/D5b). The watcher thread that already loops
on `svc_gate->wait()` gains a second condition:
`dep_watch->wait()` before `svc_gate->wait()`. If deps recover,
the FSM checks `is_disabled()` next; if not disabled, it registers.

**lwm2m-server**: When dep_down, reject Register with 5.03 (same
as enable=false). Already handled by the watcher thread's
`set_disabled` toggle.

**Tests.**
- `wifi-client/test/supervisor_test.cpp`: `DepGate_dep_down_dominates_conflict`
- `apps/test/` (existing suite continues to pass; dep gate plumbing
  is mechanically identical to D3).

### D5 — ds-cli svc list DEPENDS column

**Scope.** `modules/data-store/src/cli/ds_cli.cpp` — `svc list`
gains a `DEPENDS` column.

```
$ ds-cli svc list
NAME             ENABLE  STATE      DEPENDS     UPTIME
ds               n/a     running    -           1h12m
net.router       true    running    -           1h12m
openvpn.client   true    running    net.router  47m
lwm2m.client     true    running    net.router  1h11m
lwm2m.server     true    running    net.router  1h11m
wifi.client      false   disabled   net.router  1h12m
```

The `DEPENDS` column reads the `depends_on` array from the schema
dump (already fetched for the list via `Op::SchemaDump` from
L16/D7). Comma-separated when multiple deps exist (v1 has at most
one, but the code handles N).

**Tests.** Existing `log/L16/smoke.sh` extended with a
`ds-cli svc list` assertion that the DEPENDS column shows the
expected graph.

### D6 — DEPLOY.md + smoke

**Scope.**

- `DEPLOY.md`: add a `services.* dependency graph` section showing
  the `ds-cli svc list` output, the dependency graph as a diagram,
  and the recovery procedure ("if openvpn-client shows
  gate.reason='dep_down:net.router', check
  `ds-cli svc status net.router` first").
- `log/L17a/smoke.sh`: container-side e2e. Brings up
  ds + net-router + openvpn-client via compose (reusing L14's
  compose path). Disables net-router, asserts openvpn-client's
  `vpn.gate.reason` contains `dep_down:net.router`. Re-enables
  net-router, asserts openvpn-client resumes.

---

## 3. Acceptance for L17a

L17a closes when:

- `bash log/L17a/smoke.sh` exits 0 on a clean machine with podman.
- `ds-cli svc list` shows the DEPENDS column mapping the graph.
- DepWatch unit tests pass (D1: 5 tests).
- Gate composition tests pass (D3: 3 tests; D4: 1 test).
- Every daemon with dependencies sets `gate.reason="dep_down:<name>"`
  within one event-loop tick of a dependency going disabled.
- `services.lua` cycle detection rejects circular `depends_on`.

---

## 4. After L17a

Candidate next phases (not committed):

- **L17a-follow-up — transitive-dep chain reason.** Change
  `gate.reason` from `dep_down:B` to `dep_down:B→C` when the
  chain is A→B→C and C is the root cause. Needs each daemon to
  include its own `dep_reason` in the state value.
- **L17b — ephemeral disable.** `ds-cli svc disable --until-boot`.
- **L17c — per-key ACL.**
- **L17d — chaos/rate-limit.**
