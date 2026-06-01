# L16 — services.* enable plane TDD plan

Test-driven companion to [`plan.md`](plan.md). Same shape as
`log/L15/tdd.md` and `modules/data-store/docs/tdd.md` so the iot
repo carries one consistent RDD/TDD format.

Once D1 lands the shared `data_store::ServiceGate` helper and
`modules/data-store/schemas/services.lua`, the canonical home for
this file becomes `modules/data-store/docs/services-tdd.md` (or it
folds into the existing `modules/data-store/docs/tdd.md` as a
new section). Until then it lives next to the phase plan.

| Token              | Meaning                                              |
|--------------------|------------------------------------------------------|
| REQ-SVC-NNN        | Functional requirement                               |
| NFR-SVC-NNN        | Non-functional requirement                           |
| BUG-SVC-NNN        | Defect found during a phase (must-fix or filed FUP)  |
| SVC-D-N            | Binding decision recorded in §5                      |
| L16-Dn             | Implementation phase from `plan.md` §2               |
| FUP-L16-N          | Closed-out follow-up                                 |

---

## 1. Scope

A central `services.lua` schema, a shared `data_store::ServiceGate`
helper, per-daemon wiring (ds-server / net-router / openvpn-client
/ lwm2m-client / lwm2m-server / wifi-client), and a `ds-cli svc`
subcommand. Worker-only stop semantics — the daemon stays running,
the worker subprocess (openvpn, wpa_supplicant, udhcpc, nft) is
reaped.

Out of scope for L16:

- systemctl-level start/stop (FUP — operator escape hatch only).
- Cross-service dependency graph (FUP-L17a).
- Boot-time vs ephemeral disable distinction (FUP-L17b).
- Per-key ACL beyond filesystem DAC (FUP-L17c).
- Sub-worker granularity (`services.wifi.client.dhcp.enable`).

---

## 2. Functional requirements

| ID            | Requirement                                                                                                                                                                                       | Pri | Phase |
|---------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:--:|:-----:|
| REQ-SVC-001   | `modules/data-store/schemas/services.lua` MUST declare `services.<name>.enable` (boolean, default true) and `services.<name>.state` (string, default "running") for every gateable daemon.       | M  | D1    |
| REQ-SVC-002   | ds-server MUST auto-load `services.lua` on boot via the existing `ds-schema-dir` path; missing file is silently no-op (operators upgrading from L15 see no change until the file is deployed).   | M  | D1    |
| REQ-SVC-003   | `data_store::ServiceGate(client, "services.<name>.enable")` MUST prime its snapshot synchronously on construction (via `Client::get`), then subscribe via `Client::watch` for change events.    | M  | D1    |
| REQ-SVC-004   | `ServiceGate::enabled()` MUST return `true` when the key is absent or unset (schema default applies); MUST be thread-safe (no UB when racing the listener thread).                              | M  | D1    |
| REQ-SVC-005   | `ServiceGate::wait()` MUST block the caller until `enabled()` changes value OR `shutdown()` is called; returns the new value, or `nullopt` on shutdown.                                          | M  | D1    |
| REQ-SVC-006   | `ServiceGate::publish_state(s)` MUST issue `Client::set("services.<name>.state", s)`; failures MUST be logged via `ACE_ERROR` and MUST NOT throw.                                                | M  | D1    |
| REQ-SVC-007   | ds-server MUST publish `services.ds.state="running"` once its acceptor is up; MUST update `services.ds.uptime.sec` every 60 s via the same `ACE_Reactor` timer mechanism it uses today.         | M  | D2    |
| REQ-SVC-008   | ds-server MUST reject `set services.ds.enable <any>` with `SchemaRejected`. The schema rejection path MUST be one of: (a) key omitted from `services.lua`, OR (b) explicit `readonly=true`.    | M  | D2    |
| REQ-SVC-009   | net-router MUST honour `services.net.router.enable`. On `false`: stop iface_monitor, clear `net.iface.active=""`, run nft-teardown, publish `services.net.router.state="disabled"`.            | M  | D3    |
| REQ-SVC-010   | net-router MUST restore full operation on `true`: re-spawn iface_monitor, re-install nft state, publish `services.net.router.state="running"`. Net effect MUST equal "fresh daemon start".      | M  | D3    |
| REQ-SVC-011   | openvpn-client Supervisor MUST add a ServiceGate alongside the existing WAN gate. On `enable=false`: SIGTERM+reap the openvpn child within 5 s, publish `services.openvpn.client.state="disabled"`, set `vpn.gate.reason="disabled"`. | M | D4 |
| REQ-SVC-012   | openvpn-client gate composition MUST follow: `enable=false` dominates `wan_down`. `gate.reason="disabled"` when both are closed; never "wan_down" while disabled.                                | M  | D4    |
| REQ-SVC-013   | openvpn-client `enable=true` while WAN-down MUST publish `services.openvpn.client.state="running"` (the daemon's idle-waiting-for-WAN state) and `vpn.gate.reason="wan_down"`. No openvpn child spawned. | M | D4 |
| REQ-SVC-014   | lwm2m-client MUST honour `services.lwm2m.client.enable`. On `false` while registered: send Deregister, drop active observations, keep CoAP socket listening. On `true`: re-Register from scratch.    | M  | D5    |
| REQ-SVC-015   | lwm2m-server MUST honour `services.lwm2m.server.enable`. On `false`: stop accepting new Register requests, drop active-registrations map, keep listening socket. On `true`: accept new Registers.    | M  | D5    |
| REQ-SVC-016   | wifi-client Supervisor MUST add a ServiceGate. On `enable=false`: SIGTERM+reap wpa_supplicant + udhcpc, publish `wifi.assoc.state="disconnected"` + `services.wifi.client.state="disabled"`. Depends on L15/D6. | M | D6 |
| REQ-SVC-017   | wifi-client gate composition MUST follow: `enable=false` dominates the NM-conflict gate. `state="disabled"` takes precedence over `state="conflict"`.                                            | M  | D6    |
| REQ-SVC-018   | ds-cli MUST gain `svc <verb>` subcommands: `list`, `enable <name>`, `disable <name>`, `status <name>`. Unknown verb MUST exit non-zero with usage on stderr.                                     | M  | D7    |
| REQ-SVC-019   | `ds-cli svc list` MUST enumerate every `services.*.enable` row + matching `.state` + uptime if present. Order MUST be deterministic (alphabetical by name).                                       | M  | D7    |
| REQ-SVC-020   | `ds-cli svc list` MUST source its row set from the schema via a new `schema-dump` op on the ds-server protocol; MUST NOT hardcode the daemon list in the CLI source.                            | M  | D7    |
| REQ-SVC-021   | `ds-cli svc enable/disable <name>` MUST refuse `<name>=ds` with a non-zero exit and a message naming the substrate constraint, BEFORE wire-level rejection.                                       | S  | D7    |
| REQ-SVC-022   | `DEPLOY.md` MUST gain a "services.* control plane" section showing `ds-cli svc` examples + the systemctl-vs-ds-cli distinction.                                                                  | M  | D8    |
| REQ-SVC-023   | `bash log/L16/smoke.sh` MUST exit 0: bring up ds + net-router + openvpn-client in containers; disable openvpn-client; assert no `pgrep openvpn` match; re-enable; assert it comes back.          | M  | D8    |
| REQ-SVC-024   | Every diagnostic log added in this phase MUST use `ACE_DEBUG`/`ACE_ERROR` (per memory `feedback_ace_logging.md`). ds-cli `svc` operator output stays on stdout/stderr.                            | M  | all   |

---

## 3. Non-functional requirements

| ID           | Requirement                                                                                                                          | Pri | Phase |
|--------------|--------------------------------------------------------------------------------------------------------------------------------------|:--:|:-----:|
| NFR-SVC-001  | `enable=false` MUST take effect within one Supervisor event-loop tick of the change event landing (≤ 250 ms for openvpn, ≤ 200 ms for wifi per L15 NFR-WIFI-001). | M  | D3/D4/D6 |
| NFR-SVC-002  | A SIGTERM→SIGKILL escalation deadline of 5 s MUST apply to every worker reap on disable, matching openvpn's existing `~Process` shape. | M  | D3/D4/D6 |
| NFR-SVC-003  | A rapid enable/disable bounce (e.g. 10 transitions in 1 s) MUST NOT spawn more than one extra worker beyond the final state. Coalesce identical consecutive values; the data-store already de-dupes, but the Supervisor must too. | S  | D4    |
| NFR-SVC-004  | `services.<name>.state` MUST be written only on state transitions (matches L15 NFR-WIFI-004 pattern); no redundant repeated writes.    | M  | D3/D4/D6 |
| NFR-SVC-005  | `ServiceGate::wait()` MUST wake every waiting thread on either a change event or `shutdown()`; no missed wakeups under N concurrent waiters. | M | D1 |
| NFR-SVC-006  | The `schema-dump` op MUST return the full key registry in O(n) over the registry size; no per-key wire round-trip.                    | M  | D7    |
| NFR-SVC-007  | Every Must-priority requirement MUST be covered by at least one GoogleTest case named `SVC_<RequirementID>_…`.                       | M  | all   |
| NFR-SVC-008  | An in-tree wire-evidence transcript MUST live at `log/L16/svc-smoke.txt`, capturing the disable/enable round-trip for at least one daemon. | M  | D7/D8 |

---

## 4. Traceability matrix

| ID            | Phase | Test artifact                                                                                                                  |
|---------------|:-----:|--------------------------------------------------------------------------------------------------------------------------------|
| REQ-SVC-001   | D1    | `services_schema_test.cpp::SVC_REQ_SVC_001_schema_declares_all_daemons`                                                        |
| REQ-SVC-002   | D1    | `services_schema_test.cpp::SVC_REQ_SVC_002_auto_load_via_ds_schema_dir`                                                        |
| REQ-SVC-003   | D1    | `service_gate_test.cpp::SVC_REQ_SVC_003_constructor_primes_then_watches`                                                       |
| REQ-SVC-004   | D1    | `service_gate_test.cpp::SVC_REQ_SVC_004_absent_key_resolves_to_default_true` + `…_threadsafe_under_listener`                   |
| REQ-SVC-005   | D1    | `service_gate_test.cpp::SVC_REQ_SVC_005_wait_returns_on_change_or_shutdown`                                                    |
| REQ-SVC-006   | D1    | `service_gate_test.cpp::SVC_REQ_SVC_006_publish_state_no_throw_on_failure`                                                     |
| REQ-SVC-007   | D2    | `ds_server_test.cpp::SVC_REQ_SVC_007_ds_publishes_state_and_uptime`                                                            |
| REQ-SVC-008   | D2    | `ds_server_test.cpp::SVC_REQ_SVC_008_ds_enable_set_rejected`                                                                   |
| REQ-SVC-009   | D3    | `net-router/test/supervisor_test.cpp::SVC_REQ_SVC_009_disable_clears_active_iface_and_nft`                                     |
| REQ-SVC-010   | D3    | `net-router/test/supervisor_test.cpp::SVC_REQ_SVC_010_reenable_restores_full_operation`                                        |
| REQ-SVC-011   | D4    | `openvpn-client/test/supervisor_test.cpp::SVC_REQ_SVC_011_disable_reaps_child_within_5s`                                        |
| REQ-SVC-012   | D4    | `openvpn-client/test/supervisor_test.cpp::SVC_REQ_SVC_012_disable_dominates_wan_down`                                          |
| REQ-SVC-013   | D4    | `openvpn-client/test/supervisor_test.cpp::SVC_REQ_SVC_013_enabled_but_wan_down_publishes_running_and_wan_down_reason`           |
| REQ-SVC-014   | D5    | `lwm2m_lifecycle_test.cpp::SVC_REQ_SVC_014_disable_deregisters_and_keeps_socket`                                                |
| REQ-SVC-015   | D5    | `lwm2m_lifecycle_test.cpp::SVC_REQ_SVC_015_server_disable_drops_registrations_keeps_socket`                                    |
| REQ-SVC-016   | D6    | `wifi-client/test/supervisor_test.cpp::SVC_REQ_SVC_016_disable_reaps_wpa_and_dhcp` *(depends on L15/D6)*                       |
| REQ-SVC-017   | D6    | `wifi-client/test/supervisor_test.cpp::SVC_REQ_SVC_017_disable_dominates_nm_conflict`                                          |
| REQ-SVC-018   | D7    | `ds-cli/test/svc_cli_test.cpp::SVC_REQ_SVC_018_unknown_verb_exits_nonzero`                                                     |
| REQ-SVC-019   | D7    | `ds-cli/test/svc_cli_test.cpp::SVC_REQ_SVC_019_list_orders_alphabetically`                                                     |
| REQ-SVC-020   | D7    | `ds-cli/test/svc_cli_test.cpp::SVC_REQ_SVC_020_list_sourced_from_schema_dump`                                                  |
| REQ-SVC-021   | D7    | `ds-cli/test/svc_cli_test.cpp::SVC_REQ_SVC_021_enable_disable_ds_refused_client_side`                                          |
| REQ-SVC-022   | D8    | manual: `grep -A 30 'services\.\* control plane' DEPLOY.md`                                                                    |
| REQ-SVC-023   | D8    | `log/L16/smoke.sh` exits 0; `log/L16/svc-smoke.txt` checked in                                                                  |
| REQ-SVC-024   | all   | manual: `grep -rn 'std::cout\|std::cerr' modules/` in diff shows no new diagnostic output (ds-cli operator output exempt)        |
| NFR-SVC-001   | D3/D4/D6 | `*_supervisor_test.cpp::SVC_NFR_SVC_001_disable_takes_effect_within_one_tick`                                              |
| NFR-SVC-002   | D3/D4/D6 | `process_test.cpp::SVC_NFR_SVC_002_sigterm_then_sigkill_within_5s` (shared `/bin/sh` stand-in)                              |
| NFR-SVC-003   | D4    | `openvpn-client/test/supervisor_test.cpp::SVC_NFR_SVC_003_rapid_bounce_coalesced`                                              |
| NFR-SVC-004   | D3/D4/D6 | `*_supervisor_test.cpp::SVC_NFR_SVC_004_state_written_only_on_transition`                                                  |
| NFR-SVC-005   | D1    | `service_gate_test.cpp::SVC_NFR_SVC_005_multi_waiter_wakeup`                                                                    |
| NFR-SVC-006   | D7    | `ds_server_test.cpp::SVC_NFR_SVC_006_schema_dump_single_roundtrip`                                                              |
| NFR-SVC-007   | all   | meta: every closed phase must add the `SVC_<ID>_…` case for that phase's Must rows                                            |
| NFR-SVC-008   | D7/D8 | `log/L16/svc-smoke.txt` checked into the repo with the D8 PR                                                                    |

---

## 5. Decisions log

| Date       | ID       | Decision                                                                                                                                                                                                                                                                                       | Rationale                                                                                                                                                                                                                                | Affects             |
|------------|----------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------|
| 2026-06-01 | SVC-D1   | **Worker-only stop semantics.** `enable=false` reaps the worker subprocess (openvpn, wpa_supplicant, udhcpc, nft). The daemon (and its DsBridge) stays alive so state remains observable. systemctl stays the escape hatch for the daemon itself.                                              | Avoids needing CAP_SYS_ADMIN or a privileged sidecar to systemctl-control siblings. Reuses the openvpn-client WAN-gate pattern already proven at L13. Keeps the rootless-podman dev path working.                                       | §0, REQ-SVC-011/016 |
| 2026-06-01 | SVC-D2   | **Central `services.lua` schema** under `modules/data-store/schemas/`, not per-daemon `services.<name>.*` in each module's schema.                                                                                                                                                            | The set of services is small + known; the pattern is identical for every entry; `ds-cli svc list` wants one enumerable source. Per-daemon ownership doesn't add anything here vs the wifi.lua / vpn.lua pattern where each namespace is genuinely owned by one daemon. | §2 D1, REQ-SVC-001  |
| 2026-06-01 | SVC-D3   | **`enable=false` dominates every other gate.** WAN gate, NM-conflict gate, future deps gate — all subordinated. `gate.reason="disabled"` takes precedence over `"wan_down"` / `"conflict"`.                                                                                                  | Operators reading `gate.reason` want the most-actionable reason first. "I disabled it" is more actionable than "WAN is down". Documented in design.md so the convention is visible.                                                    | REQ-SVC-012/017     |
| 2026-06-01 | SVC-D4   | **ds-server publishes `services.ds.state`/`uptime.sec` but rejects `set services.ds.enable`.** Schema either omits the key (preferred) or marks it `readonly=true` if a uniform shape is judged worth a new schema attribute.                                                                | Self-disable of the substrate is a trap — the very socket carrying the disable command goes dead, and recovery needs a file edit + restart. The asymmetry is small enough to live with; uniformity is the next decision below.        | §0, REQ-SVC-008     |
| 2026-06-01 | SVC-D5   | **Omit `services.ds.enable` from the schema entirely.** Take path (a) of SVC-D4 alternatives. SchemaRegistry's namespace-claimed-but-key-not-declared rejection path handles this.                                                                                                          | Adding a `readonly` attribute is one more thing for the schema parser + ds-cli to know about. The asymmetry of one daemon having no enable is documented in DEPLOY.md and visible in `ds-cli svc list` (the row shows `ENABLE=n/a`).   | REQ-SVC-008         |
| 2026-06-01 | SVC-D6   | **lwm2m "disabled" = Deregister + drop observations, keep CoAP socket listening.** Not SIGTERM-the-CoAP-loop, not socket-close.                                                                                                                                                                | Preserves the daemon's primary I/O surface so re-enable doesn't need a port rebind (and a possible "address in use" race). Same shape as openvpn-client's idle-while-WAN-down state — daemon up, work paused.                          | REQ-SVC-014/015     |
| 2026-06-01 | SVC-D7   | **Shared `data_store::ServiceGate` helper, not per-daemon copy.** Lives in `modules/data-store/inc/data_store/service_gate.hpp`, linked via the existing client lib.                                                                                                                          | Six daemons need the same plumbing (prime + watch + wait + publish_state). Copy-now-share-later loses to one consistent abstraction here — the WAN-gate copy happened only because there was one consumer at the time.                | §2 D1, REQ-SVC-003  |
| 2026-06-01 | SVC-D8   | **`ds-cli svc list` enumerates via a new `schema-dump` op**, not a hardcoded daemon list in the CLI.                                                                                                                                                                                          | Adding a new gated daemon (e.g. future cellular) shouldn't require a ds-cli rebuild. `schema-dump` is independently useful for debugging schema mismatches; cost is one new protocol verb.                                            | REQ-SVC-020, NFR-SVC-006 |

---

## 6. Implementation phases

Phases come from `plan.md` §2. Each row lists the gtest case (or
manual step) that must pass before the phase PR merges. NFR-SVC-007
requires every Must row in §4 to have at least one such case in
the phase that closes it.

| Phase | Goal                                                                                                       | Closes                                                              | Risk gate                                                                                                                  | Status |
|-------|------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------|--------|
| D1    | `services.lua` + `data_store::ServiceGate` shared helper.                                                  | REQ-SVC-001..006, NFR-SVC-005                                       | `services_schema_test.cpp::SVC_REQ_SVC_001/002_*` + `service_gate_test.cpp::SVC_REQ_SVC_003..006_*`.                       | Open |
| D2    | ds-server self-publishes `services.ds.*`; rejects `set services.ds.enable`.                                 | REQ-SVC-007, REQ-SVC-008                                            | `ds_server_test.cpp::SVC_REQ_SVC_007/008_*`; wire smoke: `ds-cli set services.ds.enable false` → `SchemaRejected`.        | Open |
| D3    | net-router enable gate (clears `net.iface.active` + nft state on disable; re-installs on enable).          | REQ-SVC-009, REQ-SVC-010, NFR-SVC-001/004                           | `net-router/test/supervisor_test.cpp::SVC_REQ_SVC_009/010_*` + `SVC_NFR_SVC_001/004_*`.                                    | Open |
| D4    | openvpn-client enable gate (composes with WAN gate; disable dominates).                                    | REQ-SVC-011..013, NFR-SVC-001..004                                  | `openvpn-client/test/supervisor_test.cpp::SVC_REQ_SVC_011/012/013_*` + `SVC_NFR_SVC_003_*` rapid-bounce.                  | Open |
| D5    | lwm2m-client + lwm2m-server enable gate (Deregister + park; socket stays listening).                       | REQ-SVC-014, REQ-SVC-015                                            | `lwm2m_lifecycle_test.cpp::SVC_REQ_SVC_014/015_*`. If D5 grows past one PR, split into D5a (client) + D5b (server).        | Open |
| D6    | wifi-client enable gate (composes with NM-conflict gate). Depends on L15/D6.                               | REQ-SVC-016, REQ-SVC-017                                            | `wifi-client/test/supervisor_test.cpp::SVC_REQ_SVC_016/017_*`. Slips to "after L15 closes" if L15/D6 not landed yet.       | Open |
| D7    | `ds-cli svc list/enable/disable/status` + `schema-dump` protocol op.                                       | REQ-SVC-018..021, NFR-SVC-006/008                                   | `ds-cli/test/svc_cli_test.cpp::SVC_REQ_SVC_018..021_*` + `log/L16/svc-smoke.txt` transcript checked in.                    | Open |
| D8    | DEPLOY.md + container smoke harness (disable openvpn-client → `pgrep openvpn` empty → re-enable → comes back). | REQ-SVC-022, REQ-SVC-023                                            | `bash log/L16/smoke.sh` exits 0; DEPLOY.md grep for "services.* control plane" section.                                    | Open |

Test rules per phase (mirrored from `log/L15/tdd.md` §6):

1. **TDD shape** — every new source file ships with at least one
   `SVC_<RequirementID>_…` gtest case under the appropriate
   `modules/*/test/` directory.
2. **No skip on the closure side** — the risk-gate test must
   pass; if flaky, fix the flake before closing the phase.
3. **In-tree wire evidence** — D7/D8 lands `log/L16/svc-smoke.txt`.
4. **No new BUG-SVC-IDs without a follow-up** — defects either
   get fixed in the phase PR or filed as a `BUG-SVC-NNN` row here
   with an open FUP.

---

## 7. Risk register (test-side view)

Functional risks live in `plan.md` §1 (R1–R11). The rows here are
test-suite hazards specific to this phase.

| ID            | Risk                                                                                                                                              | Likelihood | Impact | Mitigation                                                                                                                                |
|---------------|---------------------------------------------------------------------------------------------------------------------------------------------------|:----------:|:------:|------------------------------------------------------------------------------------------------------------------------------------------|
| RISK-SVC-01   | `SVC_NFR_SVC_001_disable_takes_effect_within_one_tick` is wall-clock-sensitive; CI clock skew flakes it.                                          | M          | M      | Inject a `Clock` interface in the Supervisor (same approach RISK-WIFI-05 takes); test drives a fake clock; no `std::this_thread::sleep_for`. |
| RISK-SVC-02   | The `SchemaRejected` path for `services.ds.enable` (SVC-D5 omit-from-schema) silently regresses if a future PR adds a `services.ds.enable` row.   | L          | H      | Pin `ds_server_test.cpp::SVC_REQ_SVC_008_ds_enable_set_rejected` as a regression gate; review checklist mentions the SVC-D5 invariant.    |
| RISK-SVC-03   | `SVC_NFR_SVC_003_rapid_bounce_coalesced` (10 transitions in 1 s) races the listener thread; flaky if the data-store de-dupe drops some events.   | M          | M      | Coalescing logic lives in the Supervisor, not the listener; test drives canned event sequences (no real `Client::set` round-trips).        |
| RISK-SVC-04   | D5 lwm2m "Deregister + keep socket" path collides with the existing lifetime/reregister timer; double-Register on rapid bounce.                  | M          | M      | D5 unit tests cover: enable→false→true within < lifetime, enable→false→true after lifetime. Both cases must produce exactly one Register. |
| RISK-SVC-05   | The new `schema-dump` op leaks key-by-key implementation detail (e.g., raw Lua tables) that ds-cli then has to special-case.                      | M          | L      | D7 designs the op output as JSON `[{name, type, default, min, max, readonly?}, …]`; ds-cli renders, doesn't inspect internals.            |
| RISK-SVC-06   | `bash log/L16/smoke.sh` requires a working openvpn container that successfully binds CAP_NET_ADMIN — rootless podman on some CI runners can't.    | M          | H      | Smoke uses the fake-openvpn `/bin/sh` stand-in already established in L12 (`OPENVPN_BIN=…` override); real openvpn is operator-side only.  |
| RISK-SVC-07   | `services.<name>.state` writes from multiple Supervisor entry points (gate change, worker exit, timer) race and produce out-of-order observers.   | L          | M      | All `publish_state` calls funnel through `ServiceGate::publish_state`, which serialises under the gate's mutex. Documented as an invariant. |

---

## 8. Test-suite map

Tests land under the appropriate module's `test/` directory and
link the gtest target shape that module already uses.

Mock surfaces:

- `ServiceGate` accepts an injectable `data_store::Client`
  (default = real ACE socket) so D1 tests drive the listener
  with a fake client.
- Each Supervisor accepts an injected `Clock` (per RISK-SVC-01)
  + an injected `Process` (already established in openvpn-client
  + planned for wifi-client) so D3/D4/D6 tests run without root
  or netlink access.
- `ds_server_test.cpp` reuses the existing in-process server
  harness; `schema-dump` is exercised over a `socketpair(2)`
  loopback like the rest of the protocol tests.

Required test files (one per phase that produces one):

- `modules/data-store/test/services_schema_test.cpp` — D1
  schema-load cases.
- `modules/data-store/test/service_gate_test.cpp` — D1
  ServiceGate cases.
- `modules/data-store/test/ds_server_test.cpp` — D2 + D7
  additions (state publish, enable-rejection, schema-dump).
- `modules/net/router/test/supervisor_test.cpp` — D3 wiring +
  teardown cases.
- `modules/openvpn/client/test/supervisor_test.cpp` — D4 wiring
  + composition + bounce cases.
- `apps/test/lwm2m_lifecycle_test.cpp` — D5 Deregister + park
  cases.
- `modules/wan/wifi/client/test/supervisor_test.cpp` — D6 cases
  (depends on L15/D6).
- `modules/data-store/ds-cli/test/svc_cli_test.cpp` — D7
  subcommand parse + behavior cases.

---

## 9. Related docs

- [`plan.md`](plan.md) — forward-looking L16 phase plan; this
  TDD is its requirements/test-mapping companion.
- `modules/data-store/docs/tdd.md` — RDD/TDD format mirrored
  here; SVC keys will likely fold into this doc once D1 lands.
- `log/L15/tdd.md` — wifi-client TDD; shares the cross-phase D6
  dependency (L15/D6 must close before L16/D6 can).
- `modules/openvpn/client/src/supervisor.hpp` — the WAN-gate
  pattern this phase generalises.
