# L15 — wifi-client TDD plan

Test-driven companion to [`plan.md`](plan.md). Same shape as
`modules/data-store/docs/tdd.md` so the iot repo carries one
consistent RDD/TDD format across modules.

Once D1 lands and `modules/wan/wifi/client/docs/` exists, this
file migrates to `modules/wan/wifi/client/docs/tdd.md`. Until
then it lives next to the phase plan.

| Token              | Meaning                                              |
|--------------------|------------------------------------------------------|
| REQ-WIFI-NNN       | Functional requirement                               |
| NFR-WIFI-NNN       | Non-functional requirement                           |
| BUG-WIFI-NNN       | Defect found during a phase (must-fix or filed FUP)  |
| WIFI-D-N           | Binding decision recorded in §5                      |
| L15-Dn             | Implementation phase from `plan.md` §2               |
| FUP-L15-N          | Closed-out follow-up                                 |

---

## 1. Scope

A new daemon `wifi-client` under `modules/wan/wifi/client/` that
owns the `wpa_supplicant(8)` lifecycle for one wifi iface, drives
scan + association, spawns a sibling DHCP client once associated,
and publishes `wifi.*` state into the data store. Single radio,
PSK only, no EAP, no AP mode, no cellular — see `plan.md` §0
non-goals.

Out of scope for L15:

- 802.1X / EAP / enterprise credentials (FUP — separate phase).
- AP mode, cellular, multi-radio (FUP — separate modules).
- NetworkManager coexistence: detect + refuse only (FUP-L16e).
- PSK secret hardening — plaintext in ds-server, same posture as
  `vpn.cert.path` (FUP-L15-1 → L16a vault).

---

## 2. Functional requirements

| ID            | Requirement                                                                                                                                                                              | Pri | Phase |
|---------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:--:|:-----:|
| REQ-WIFI-001  | The daemon MUST build as a standalone cmake target `wifi-client` under `modules/wan/wifi/client/` and link via `apps/CMakeLists.txt::add_subdirectory`, with no edits to sibling modules. | M  | D1    |
| REQ-WIFI-002  | The CLI MUST accept `--ds-sock`, `--wpa`, `--iface`, `--ctrl-dir`, `--dump`, `--once`, `--help`. Unknown flags MUST exit non-zero with a usage line on stderr.                            | M  | D1    |
| REQ-WIFI-003  | `--dump` MUST list every read/write key the daemon touches in `wifi.*` and exit 0 without contacting ds-server.                                                                          | S  | D1    |
| REQ-WIFI-004  | ds-server MUST auto-load `modules/wan/wifi/client/schemas/wifi.lua` and reject `set` calls that violate the per-key `(type, default, min?, max?)` spec with `SchemaRejected`.            | M  | D2    |
| REQ-WIFI-005  | All read keys from `plan.md` §D2 table MUST have schema entries with the documented defaults; `wifi.networks` defaults to `"[]"` (empty JSON array).                                     | M  | D2    |
| REQ-WIFI-006  | All write keys from `plan.md` §D2 table MUST have schema entries; `wifi.assoc.state` MUST be one of the documented string enum values; integer keys MUST have type=integer.              | M  | D2    |
| REQ-WIFI-007  | `DsBridge` MUST prime its cache by `get`-ing every read key on `start()`, then `register` for change events. Missing keys MUST fall back to the schema default, not error.                | M  | D3    |
| REQ-WIFI-008  | `DsBridge` setters (`set_assoc_state`, `set_signal_rssi`, …) MUST log a non-fatal `ACE_ERROR` and return without throwing when the underlying `Client::set` fails.                       | M  | D3    |
| REQ-WIFI-009  | `DsBridge::on_change` registration MUST accept a null callback to clear a previously-registered watch without disconnecting from ds-server.                                              | S  | D3    |
| REQ-WIFI-010  | `ctrl::Client::connect(path)` MUST open an `AF_UNIX` stream to the wpa_supplicant control socket and issue `ATTACH` to enable unsolicited events.                                        | M  | D4    |
| REQ-WIFI-011  | `ctrl::Client::request(cmd, reply)` MUST send `cmd\n`, return `true` on a non-`FAIL` reply, populate `reply` with the raw payload, and `false` on either `FAIL` or socket error.         | M  | D4    |
| REQ-WIFI-012  | The CTRL-EVENT parser MUST classify each of: SCAN-STARTED, SCAN-RESULTS, CONNECTED, DISCONNECTED, ASSOC-REJECT, AUTH-REJECT, TERMINATING into the documented `CtrlEvent::Kind`.          | M  | D4    |
| REQ-WIFI-013  | `CtrlEvent::Kind::Connected` MUST populate `ssid` + `bssid` from the event line; `*Reject` and `Disconnected` MUST populate `reason`. Unparseable lines map to `Kind::Unknown`, no throw. | M  | D4    |
| REQ-WIFI-014  | `Process::spawn` MUST return `true` on success with `pid()` > 0, `false` on `ACE_Process::spawn` failure with `pid()==0`. Destructor MUST reap any live child (no zombies).              | M  | D5    |
| REQ-WIFI-015  | The wpa_supplicant config writer MUST translate a `wifi.networks` JSON array into a valid `wpa_supplicant.conf` with one `network={}` block per entry, ordered by descending priority.   | M  | D5    |
| REQ-WIFI-016  | A `wifi.networks` entry with `key_mgmt="NONE"` MUST emit `key_mgmt=NONE` and omit `psk`; otherwise `psk="..."` is required and missing-psk MUST surface as `bad_networks_json: …`.        | M  | D5    |
| REQ-WIFI-017  | The DHCP-client picker MUST honour `wifi.dhcp.client`: `"udhcpc"`, `"dhclient"`, or `"auto"` (probe in that order). Selection result MUST be logged once at startup.                      | M  | D5    |
| REQ-WIFI-018  | The `Lifecycle` FSM MUST move through `disconnected → scanning → associating → 4way → connected` only via the documented `CtrlEvent` triggers; any other transition MUST stay in-state. | M  | D6    |
| REQ-WIFI-019  | On entering `connected`, the supervisor MUST spawn the DHCP child and publish `wifi.dhcp.state="requesting"`; on leaving `connected` it MUST reap DHCP and publish `wifi.dhcp.state="exited"`. | M  | D6    |
| REQ-WIFI-020  | An additive `wifi.networks` change (new SSID added, every existing entry untouched) MUST issue `RECONFIGURE` to wpa_supplicant. Any non-additive change MUST trigger a full restart.       | M  | D6    |
| REQ-WIFI-021  | A bump (any change) to `wifi.scan.request` MUST issue a `SCAN` command via the ctrl client within one event-loop tick.                                                                   | M  | D6    |
| REQ-WIFI-022  | If `systemctl is-active NetworkManager` is yes or `<wifi.ctrl.dir>/<wifi.iface>` already exists at startup, the daemon MUST publish `wifi.assoc.state="conflict"` + a descriptive `wifi.last.error` and refuse to spawn wpa_supplicant. | M | D6 |
| REQ-WIFI-023  | The packaging Containerfile + dev `docker/Dockerfile` MUST install `wpasupplicant udhcpc wireless-tools`; the systemd unit MUST ship `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`.    | M  | D7    |
| REQ-WIFI-024  | The `packaging/iot-entrypoint.sh` `IOT_ROLE=wifi` branch MUST `exec wifi-client --ds-sock=…` and propagate exit codes verbatim (no swallow).                                              | M  | D7    |
| REQ-WIFI-025  | `bash log/L15/smoke.sh` MUST exit 0 on a clean machine with `podman` + `bash` only; no host wlan stack, no real radio. Output MUST capture the wifi.assoc.state transition to `connected`. | M  | D8    |
| REQ-WIFI-026  | All server-emitting diagnostic logs in `wifi-client` MUST use `ACE_DEBUG` / `ACE_ERROR` (per memory `feedback_ace_logging.md`). Operator-facing CLI output (`--dump`, `--help`) stays on stdout. | M | D1+    |

---

## 3. Non-functional requirements

| ID           | Requirement                                                                                                                          | Pri | Phase |
|--------------|--------------------------------------------------------------------------------------------------------------------------------------|:--:|:-----:|
| NFR-WIFI-001 | The supervisor event loop MUST poll `ctrl::Client::recv_event` with ≤ 200 ms timeout so `wifi.networks` change + signal events stay responsive. | S  | D6 |
| NFR-WIFI-002 | `wifi.signal.rssi` MUST be written to ds-server at most once per 5 s; rapid `SIGNAL_POLL` replies in between MUST be coalesced.       | M  | D6 |
| NFR-WIFI-003 | `wifi.scan.results` MUST be capped to the strongest `wifi.scan.max.results` entries (default 20) ordered by descending signal level. | M  | D6 |
| NFR-WIFI-004 | `wifi.assoc.state` MUST be written only on state transitions, never repeated on identical re-entries.                                | M  | D6 |
| NFR-WIFI-005 | The daemon MUST tolerate ≥ 50 CTRL-EVENT lines/sec without dropping any event (parser is O(line-length)).                            | S  | D4 |
| NFR-WIFI-006 | `Process` destructor MUST reap a live child within 5 s of `~Process()` (SIGTERM, then SIGKILL on timeout). No orphan wpa_supplicant. | M  | D5 |
| NFR-WIFI-007 | Every Must-priority requirement MUST be covered by at least one GoogleTest case named `WIFI_<RequirementID>_…`.                       | M  | all   |
| NFR-WIFI-008 | An in-tree wire-evidence transcript MUST live at `log/L15/wifi-smoke.txt`, capturing the fake-wpa control session for the smoke run. | M  | D8    |

---

## 4. Traceability matrix

Every Must-priority requirement maps to its phase and the gtest case
that closes its risk gate. Rows close as their phase merges.

| ID            | Phase | Test artifact                                                                                                |
|---------------|:-----:|--------------------------------------------------------------------------------------------------------------|
| REQ-WIFI-001  | D1    | manual: `cmake --build` produces `wifi-client` after `add_subdirectory(modules/wan/wifi/client)`             |
| REQ-WIFI-002  | D1    | `main_test.cpp::WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown`                                      |
| REQ-WIFI-003  | D1    | `main_test.cpp::WIFI_REQ_WIFI_003_dump_lists_keys_without_ds`                                                 |
| REQ-WIFI-004  | D2    | `schema_test.cpp::WIFI_REQ_WIFI_004_bad_type_rejected_by_ds`                                                  |
| REQ-WIFI-005  | D2    | `schema_test.cpp::WIFI_REQ_WIFI_005_read_keys_have_defaults`                                                  |
| REQ-WIFI-006  | D2    | `schema_test.cpp::WIFI_REQ_WIFI_006_write_keys_typed`                                                         |
| REQ-WIFI-007  | D3    | `ds_bridge_test.cpp::WIFI_REQ_WIFI_007_start_primes_then_registers`                                          |
| REQ-WIFI-008  | D3    | `ds_bridge_test.cpp::WIFI_REQ_WIFI_008_setter_logs_on_failure_no_throw`                                      |
| REQ-WIFI-009  | D3    | `ds_bridge_test.cpp::WIFI_REQ_WIFI_009_on_change_null_callback_resets`                                       |
| REQ-WIFI-010  | D4    | `ctrl_test.cpp::WIFI_REQ_WIFI_010_connect_sends_attach`                                                      |
| REQ-WIFI-011  | D4    | `ctrl_test.cpp::WIFI_REQ_WIFI_011_request_classifies_ok_and_fail`                                            |
| REQ-WIFI-012  | D4    | `ctrl_test.cpp::WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds`                                         |
| REQ-WIFI-013  | D4    | `ctrl_test.cpp::WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason`                                   |
| REQ-WIFI-014  | D5    | `process_test.cpp::WIFI_REQ_WIFI_014_spawn_pid_and_destructor_reaps` (uses `/bin/sh` stand-in)                |
| REQ-WIFI-015  | D5    | `process_test.cpp::WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority`                                     |
| REQ-WIFI-016  | D5    | `process_test.cpp::WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates`                                   |
| REQ-WIFI-017  | D5    | `process_test.cpp::WIFI_REQ_WIFI_017_dhcp_picker_honours_schema_key`                                         |
| REQ-WIFI-018  | D6    | `lifecycle_test.cpp::WIFI_REQ_WIFI_018_fsm_documented_transitions_only`                                      |
| REQ-WIFI-019  | D6    | `supervisor_test.cpp::WIFI_REQ_WIFI_019_connected_spawns_dhcp_and_publishes`                                  |
| REQ-WIFI-020  | D6    | `supervisor_test.cpp::WIFI_REQ_WIFI_020_additive_change_uses_reconfigure`                                    |
| REQ-WIFI-021  | D6    | `supervisor_test.cpp::WIFI_REQ_WIFI_021_scan_request_bump_issues_scan`                                       |
| REQ-WIFI-022  | D6    | `supervisor_test.cpp::WIFI_REQ_WIFI_022_nm_active_or_socket_exists_yields_conflict`                          |
| REQ-WIFI-023  | D7    | manual: `grep wpasupplicant packaging/Containerfile docker/Dockerfile` + `grep CAP_NET_ADMIN packaging/systemd/iot-wifi-client.service` |
| REQ-WIFI-024  | D7    | manual: `IOT_ROLE=wifi bash packaging/iot-entrypoint.sh --dump` exits 0; bad arg propagates non-zero          |
| REQ-WIFI-025  | D8    | `log/L15/smoke.sh` exits 0; `log/L15/wifi-smoke.txt` shows the connect transition                            |
| REQ-WIFI-026  | D1+   | manual: `grep -rn 'std::cout\|std::cerr' modules/wan/wifi/client/src/` shows no diagnostic output (CLI usage exempt) |
| NFR-WIFI-002  | D6    | `supervisor_test.cpp::WIFI_NFR_WIFI_002_rssi_coalesced_to_5s`                                                |
| NFR-WIFI-003  | D6    | `supervisor_test.cpp::WIFI_NFR_WIFI_003_scan_results_capped_and_sorted`                                      |
| NFR-WIFI-004  | D6    | `supervisor_test.cpp::WIFI_NFR_WIFI_004_assoc_state_written_only_on_transition`                              |
| NFR-WIFI-006  | D5    | `process_test.cpp::WIFI_NFR_WIFI_006_destructor_reaps_within_5s`                                              |
| NFR-WIFI-007  | all   | meta: every closed phase must add the `WIFI_<ID>_…` case for that phase's Must rows                          |
| NFR-WIFI-008  | D8    | `log/L15/wifi-smoke.txt` checked into the repo with the D8 PR                                                |

---

## 5. Decisions log

Binding decisions; new ones get appended as L15 progresses. Each
row carries the date so future readers can see the order.

| Date       | ID       | Decision                                                                                                                                                                                                                                                                                                  | Rationale                                                                                                                                                                                                                                | Affects             |
|------------|----------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------|
| 2026-06-01 | WIFI-D1  | **Single radio, PSK only, v1.** `wifi.iface` is one string; `wifi.networks` is PSK or open. EAP, multi-radio, AP mode all FUP.                                                                                                                                                                            | Smallest scope that closes the WAN-gate chain end-to-end without dragging in supplicant EAP plumbing or hostapd. Lets D6/D8 focus on the lifecycle, not the credential model.                                                            | §1, REQ-WIFI-005    |
| 2026-06-01 | WIFI-D2  | **Schema lives at `modules/wan/wifi/client/schemas/wifi.lua`** and is auto-loaded by ds-server's `ds-schema-dir`. No edits to ds-server. Same pattern data-store DS-D9 established.                                                                                                                       | Per-client schema ownership matches the rest of the iot config tree; daemons are responsible for their own keys. Adding wifi keys never touches ds-server's TU.                                                                          | §2, REQ-WIFI-004    |
| 2026-06-01 | WIFI-D3  | **No persistent NetworkManager coexistence in v1.** Detect on startup, refuse with `wifi.assoc.state="conflict"`. Cooperative D-Bus handoff is FUP-L16e.                                                                                                                                                  | NM hand-off via D-Bus is a real piece of work; refusing cleanly is one `system("systemctl is-active NetworkManager")` + a clear log. Operators get a deterministic failure mode, not a half-managed iface.                              | REQ-WIFI-022        |
| 2026-06-01 | WIFI-D4  | **Plaintext PSK in ds-server, deferred to FUP-L15-1.** Same posture L12 took for `vpn.cert.path` / `vpn.ca.path`.                                                                                                                                                                                         | A secrets vault is its own phase (encrypt at rest in ds-server + decrypt on demand). Punting keeps L15 single-concern; documented in design.md per §1.                                                                                   | §1, RISK-WIFI-04    |
| 2026-06-01 | WIFI-D5  | **DHCP-client probe order is `udhcpc` → `dhclient` when `wifi.dhcp.client="auto"`.** Both probed at startup, first one found wins. `"udhcpc"` / `"dhclient"` force the choice.                                                                                                                            | udhcpc is the busybox-image default; dhclient is the Debian/Ubuntu default. `"auto"` covers both image targets without operator config. Forced choices give operators an escape hatch when probe heuristics misfire.                    | REQ-WIFI-017        |
| 2026-06-01 | WIFI-D6  | **`wpa_supplicant` runs foreground (no `-B`), `ACE_Process` owns the pid.** Mirrors openvpn-client. Generated conf written via the same `write_temp_config` + `mkstemp` pattern; copy from openvpn-client for v1, lift to a shared header if a third caller appears.                                       | RAII reaps deterministically. Background daemonisation makes lifecycle ownership ambiguous and would force a separate pid-file dance. Copy-now-share-later avoids speculative abstraction.                                              | §2 D5, REQ-WIFI-014 |
| 2026-06-01 | WIFI-D7  | **Smoke harness uses `fake-wpa.sh`, not a real radio.** L15-acceptance is "exits 0 on a clean machine with podman"; real radio coverage is operator-side.                                                                                                                                                | Same rationale L14 used for openvpn (rootless podman can't bind to host wlan, no AP to test against). Fake supplicant exercises the protocol surface the daemon depends on; real-radio testing moves to a future on-device harness.    | REQ-WIFI-025, §8    |
| 2026-06-01 | WIFI-D8  | **Diagnostic logging via ACE_DEBUG/ACE_ERROR everywhere, including the wifi-client binary.** Per memory `feedback_ace_logging.md`. CLI usage output exempt (operator/script contract).                                                                                                                    | Matches the iot binary's discipline post-DS-D10. One format, one redirect path. ds-cli's stdout-as-contract carve-out applies to the `--dump` / `--help` flags here too, not to runtime logs.                                          | REQ-WIFI-026        |

---

## 6. Implementation phases

Phases come from `plan.md` §2. Risk-gate column lists the gtest case
(or manual step) that must pass before the phase PR merges. NFR-WIFI-007
requires every Must row in §4 to have at least one such case in the
phase that closes it.

| Phase | Goal                                                                                                            | Closes                                                                                  | Risk gate                                                                                                                       | Status |
|-------|-----------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|--------|
| D1    | Module scaffold + `wifi-client` binary + CLI (`--ds-sock`/`--wpa`/`--iface`/`--ctrl-dir`/`--dump`/`--once`/`--help`). | REQ-WIFI-001, REQ-WIFI-002, REQ-WIFI-003, REQ-WIFI-026 (initial sweep)                  | `main_test.cpp::WIFI_REQ_WIFI_002_*` + `WIFI_REQ_WIFI_003_*`; `cmake --build` produces `wifi-client` after wiring `apps/CMakeLists.txt`. | Open |
| D2    | `schemas/wifi.lua` + `docs/design.md` (state machine, NM-conflict posture, plaintext-PSK posture).              | REQ-WIFI-004, REQ-WIFI-005, REQ-WIFI-006                                                | `schema_test.cpp::WIFI_REQ_WIFI_00{4,5,6}_*`; wire smoke: `ds-cli set wifi.iface 7` → `SchemaRejected`.                          | Open |
| D3    | `DsBridge` for `wifi.*` (prime + watch + setters + on_change reset).                                            | REQ-WIFI-007, REQ-WIFI-008, REQ-WIFI-009                                                | `ds_bridge_test.cpp::WIFI_REQ_WIFI_007/008/009_*`.                                                                              | Open |
| D4    | `ctrl::Client` (ACE LSOCK wrapper) + event parser.                                                              | REQ-WIFI-010, REQ-WIFI-011, REQ-WIFI-012, REQ-WIFI-013, NFR-WIFI-005                    | `ctrl_test.cpp::WIFI_REQ_WIFI_010/011/012/013_*`.                                                                               | Open |
| D5    | `Process` RAII wrapper + wpa_supplicant.conf writer + DHCP-client picker.                                       | REQ-WIFI-014, REQ-WIFI-015, REQ-WIFI-016, REQ-WIFI-017, NFR-WIFI-006                    | `process_test.cpp::WIFI_REQ_WIFI_014/015/016/017_*` + `WIFI_NFR_WIFI_006_*` (5 s reap deadline).                                | Open |
| D6    | `Lifecycle` FSM (pure) + `Supervisor` (impure) — connects ctrl + process + DsBridge.                            | REQ-WIFI-018..022, NFR-WIFI-001..004                                                    | `lifecycle_test.cpp::WIFI_REQ_WIFI_018_*` + `supervisor_test.cpp::WIFI_REQ_WIFI_019..022_*` + `WIFI_NFR_WIFI_002/003/004_*`.    | Open |
| D7    | Packaging (Containerfile, dev Dockerfile, systemd unit, entrypoint role) + DEPLOY.md.                          | REQ-WIFI-023, REQ-WIFI-024                                                              | Manual `grep` checks in §4; `IOT_ROLE=wifi packaging/iot-entrypoint.sh --dump` exits 0.                                          | Open |
| D8    | `log/L15/smoke.sh` + `fake-wpa.sh` stand-in + `log/L15/wifi-smoke.txt` transcript.                              | REQ-WIFI-025, NFR-WIFI-008                                                              | `bash log/L15/smoke.sh` exits 0 on a clean machine with podman; transcript checked in.                                           | Open |

Test rules per phase (mirrored from data-store §6):

1. **TDD shape** — every new source file ships with at least one
   `WIFI_<RequirementID>_…` gtest case in `modules/wan/wifi/client/test/`.
2. **No skip on the closure side** — the risk-gate test must pass; if
   flaky, fix the flake before closing the phase, don't `GTEST_SKIP`.
3. **In-tree wire evidence** — D8 lands `log/L15/wifi-smoke.txt`; D4
   may additionally check in canned event-trace fixtures under
   `modules/wan/wifi/client/test/data/`.
4. **No new BUG-WIFI-IDs without a follow-up** — defects either get
   fixed in the phase PR or filed as a `BUG-WIFI-NNN` row here with
   an open FUP.

---

## 7. Risk register (test-side view)

Functional risks live in `plan.md` §1 (R1–R11). The rows here are
risks that show up *as test-suite hazards* — flakiness sources, fake
fidelity gaps, environment dependencies the gtest harness must paper
over.

| ID            | Risk                                                                                                                                | Likelihood | Impact | Mitigation                                                                                                                              |
|---------------|-------------------------------------------------------------------------------------------------------------------------------------|:----------:|:------:|----------------------------------------------------------------------------------------------------------------------------------------|
| RISK-WIFI-01  | `process_test.cpp` flakes on CI when `/bin/sh` startup is slow.                                                                     | L          | M      | Reap deadline = 5 s (NFR-WIFI-006), well above kernel fork+exec on any sane runner; pin the stand-in to `/bin/sh` (not a heavier shell). |
| RISK-WIFI-02  | `fake-wpa.sh` drifts from real wpa_supplicant control-protocol shape, so D8 smoke passes against a fiction.                         | M          | H      | Capture a real `wpa_cli` transcript once at D4 + check the canned event lines into `modules/wan/wifi/client/test/data/`; fake replays exactly those bytes. Document the capture command in `docs/design.md`. |
| RISK-WIFI-03  | `wpa_supplicant` not in the dev image → D8 still "passes" via fake but D7 packaging never gets exercised.                            | M          | M      | D7 PR checklist includes `podman build` + `which wpa_supplicant` inside the image; CI runs `docker build` so the apt step can't silently regress. |
| RISK-WIFI-04  | Plaintext PSK leaks into committed test fixtures.                                                                                   | L          | H      | Fixtures use the literal SSIDs `TestAP`/`OpenAP` + the literal PSK `"correcthorse"` everywhere; pre-commit grep gate (FUP-L15-2) blocks new strings that look like real PSKs. |
| RISK-WIFI-05  | `wifi.signal.rssi` 5 s coalescing test relies on wall-clock; CI clock skew flakes it.                                               | M          | M      | Inject a `Clock` interface in `Supervisor`; test drives a fake clock. No `std::this_thread::sleep_for` in NFR-WIFI-002 test.            |
| RISK-WIFI-06  | The supervisor's "additive vs. non-additive" `wifi.networks` classifier mis-classifies edge cases (reorder, priority bump).         | M          | M      | D6 unit tests cover: pure-add (additive), pure-remove (non-additive), psk-change (non-additive), priority-only-change (TBD — record as WIFI-D9 once D6 design lands). |

---

## 8. Test-suite map

Tests land under `modules/wan/wifi/client/test/` and link the same
gtest target shape openvpn-client uses (one binary per source file
in CTest, or one combined binary — match whatever openvpn-client's
`CMakeLists.txt` picked at PR #50 review time).

Mock surfaces:

- `DsBridge` accepts an injectable `data_store::Client` (default =
  real ACE socket) so `WIFI_REQ_WIFI_007/008/009` can drive the
  setter-failure path without a live ds-server.
- `ctrl::Client` parser is split from the socket so
  `WIFI_REQ_WIFI_012/013` are pure parse tests with canned event
  strings — no `socketpair(2)` needed.
- `Process` is parameterised by binary path so all D5 tests run
  `/bin/sh -c …` stand-ins; no real wpa_supplicant.
- `Supervisor` takes injected `DsBridge`, `ctrl::Client`, `Process`,
  and `Clock` (per RISK-WIFI-05) so every D6 row is exercisable
  without root or netlink access.

Required test files (one per phase that produces one):

- `main_test.cpp` — D1 CLI parse cases.
- `schema_test.cpp` — D2 schema enforcement via ds-server's
  SchemaRegistry (linked from the data-store test harness).
- `ds_bridge_test.cpp` — D3 cache-prime + setter-error cases.
- `ctrl_test.cpp` — D4 protocol cases (parser + request/reply).
- `process_test.cpp` — D5 spawn/reap + config-writer cases.
- `lifecycle_test.cpp` — D6 pure FSM cases.
- `supervisor_test.cpp` — D6 wiring cases (DHCP spawn, coalescing,
  additive-change classifier, conflict-detection).

---

## 9. Related docs

- [`plan.md`](plan.md) — forward-looking L15 phase plan; this TDD
  is its requirements/test-mapping companion.
- `modules/data-store/docs/tdd.md` — RDD/TDD format mirrored here.
- `modules/openvpn/client/` — closest analog; the `DsBridge`,
  `Process`, `Lifecycle`, `Supervisor` shapes copy from it.
- `modules/net/router/docs/` — WAN-gate consumer; once L15 lands
  it observes `wifi.assoc.state="connected"` indirectly via the
  kernel iface flapping OPER UP.
