# L15 Plan — wifi-client (wpa_supplicant + DHCP)

> Forward-looking phase plan. Same shape as L11/L12/L13. Greenfield
> `modules/wan/wifi/client/` module — owns the wpa_supplicant
> lifecycle for one wifi iface, drives scan + association, and
> spawns a sibling DHCP child once associated. Publishes scan
> results, signal, and association state into the data store under
> the `wifi.*` namespace.
>
> Slots in cleanly above L13 (net-router) and the WAN-gate work
> (PR #50): once wifi-client associates and DHCP lands, the kernel
> brings `wlan0` OPER UP, net-router's iface_monitor picks it up
> and writes `net.iface.active`, and openvpn-client (now gated on
> `net.iface.active`) springs into life. Closing the loop.
>
> **Status (2026-06-01):** plan only.

---

## 0. Goal

A new daemon `wifi-client` that:

1. Subscribes to the operator-configured `wifi.networks` JSON (list
   of `{ssid, psk, priority}`) and a target `wifi.iface` (default
   `wlan0`).
2. Spawns `wpa_supplicant(8)` with a generated config + control
   socket; drives scan + association via the local control protocol.
3. Spawns `udhcpc` (busybox) or `dhclient` against the same iface
   once `CTRL-EVENT-CONNECTED` arrives; reaps on disconnect.
4. Publishes back: `wifi.assoc.state`, `wifi.assoc.ssid`,
   `wifi.assoc.bssid`, `wifi.signal.rssi`, `wifi.scan.results`
   (JSON), `wifi.dhcp.state`, `wifi.dhcp.ip`.
5. Honours `wifi.scan.request` (operator-triggered scan) and
   regenerates the wpa_supplicant config when `wifi.networks`
   changes.

Closes the chain: operator sets `wifi.networks` → wifi-client
brings wlan0 up → net-router sees `wlan0` OPER UP → publishes
`net.iface.active="wlan0"` → openvpn-client's WAN gate fires →
VPN tunnel comes up. Whole flow on commodity hardware with no
NetworkManager dependency.

### Non-goals (first-cut scope)

- **EAP / 802.1X / enterprise creds.** PSK only in v1.
  `wifi.networks[].psk` is the only credential field; EAP is FUP.
- **Cellular / WWAN.** Future sibling at `modules/wan/cellular/`.
- **AP mode.** Reserved path `modules/wan/wifi/ap/` for a future
  phase; out of scope here.
- **Multiple radios.** Single `wifi.iface` in v1. A second wlan
  card needs a second daemon instance + a second `wifi.*`
  namespace shard, or schema generalisation — FUP.
- **NetworkManager coexistence.** Daemon assumes nothing else is
  managing the iface. If `systemctl is-active NetworkManager` is
  yes, the daemon logs + refuses to start. Cooperative mode is FUP.
- **Roaming / 802.11r / BSS-transition.** wpa_supplicant's own
  defaults; no daemon-level roaming logic.
- **PSK secret hardening.** Credentials live in the data-store in
  plaintext, same as `vpn.ca.path` is a plain path. A secrets-vault
  store is FUP-L15-1 (see §4).
- **Scan-result deduplication / history.** We publish the latest
  results; no rolling window.

---

## 1. Risk register

| ID  | Risk                                                                                              | Mitigation                                                                                                                |
|-----|---------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|
| R1  | `wpa_supplicant` + `udhcpc` not in dev/runtime images                                              | D7 appends `wpasupplicant udhcpc wireless-tools` to docker/Dockerfile + packaging/Containerfile.                          |
| R2  | Control-socket path varies (`/run/wpa_supplicant` vs `/var/run/wpa_supplicant`)                    | Schema key `wifi.ctrl.dir` defaults to `/run/wpa_supplicant`; daemon `mkdir -p` on startup so the socket lands where it expects. |
| R3  | Needs `CAP_NET_ADMIN` + `CAP_NET_RAW` (scan)                                                        | Systemd unit ships `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`. Same `DynamicUser=yes` shape openvpn-client uses.       |
| R4  | NetworkManager or systemd-networkd already owns the iface                                          | Startup probe: `systemctl is-active NetworkManager` + check for existing `/run/wpa_supplicant/<iface>` socket. Refuse with a clear log + `wifi.assoc.state="conflict"`. |
| R5  | Plaintext PSK in ds-server is visible to anyone with the socket                                    | Document in `docs/design.md`; file FUP-L15-1 for a secrets vault. Same posture L12 took for `vpn.cert.path`.                |
| R6  | Scan results inflate the data store (50+ networks × 80 bytes each)                                 | Cap to 20 strongest at the daemon side (configurable via `wifi.scan.max.results`, default 20). JSON-encoded into one string. |
| R7  | Control-socket events arrive faster than ds-server can absorb writes                               | Coalesce: write `wifi.assoc.state` only on transitions, `wifi.signal.rssi` at most once per 5 s, `wifi.scan.results` once per `CTRL-EVENT-SCAN-RESULTS`. |
| R8  | DHCP race: `udhcpc` started before kernel assigns IP, exits immediately                            | Wait for `CTRL-EVENT-CONNECTED` (not just `CTRL-EVENT-ASSOC`); then `ip link set <iface> up` if not already; then spawn udhcpc. udhcpc has its own retry loop. |
| R9  | `wifi.networks` change while connected — bouncing the iface drops the WAN gate downstream          | Soft path: if the change is *additive* (new SSID added, existing untouched), regenerate config + `RECONFIGURE`. Otherwise full restart. Log the choice. |
| R10 | wpa_supplicant binary path varies (`/usr/sbin/wpa_supplicant` vs `/sbin/`)                          | `--wpa=PATH` CLI override + schema default `/usr/sbin/wpa_supplicant`. Mirrors openvpn-client's `--openvpn=PATH`.            |
| R11 | DHCP client choice (`udhcpc` vs `dhclient`) depends on image                                       | Probe at startup; honour `wifi.dhcp.client` schema key (`"udhcpc"` / `"dhclient"` / `"auto"`, default `"auto"`).                |

---

## 2. D-items

### D1 — plan + scaffold

**Scope.** This file + module skeleton.

```
modules/wan/wifi/client/
├── CMakeLists.txt
├── inc/
│   └── wifi_client.hpp            v0 public API (run_daemon, v0_dump_wifi_keys)
├── src/
│   ├── main.cpp                   CLI parse + entry
│   └── main_impl.cpp              v0_dump_wifi_keys scaffold
├── schemas/wifi.lua               (D2 fills this in)
├── test/                          gtest suite (added per D)
└── docs/design.md                 (D2 fills in)
```

Sibling placeholder `modules/wan/wifi/ap/` not created in L15 —
mentioned here so the `modules/wan/` namespace's intent is clear:
one subdir per radio class (`wifi/`, future `cellular/`), one
subsubdir per mode (`client/`, `ap/`).

Binary: `wifi-client`. Standalone cmake module; gets added under
`apps/CMakeLists.txt::add_subdirectory` alongside data-store +
openvpn + net.

CLI (mirrors openvpn-client):

```
wifi-client [--ds-sock=PATH] [--wpa=PATH] [--iface=NAME]
            [--ctrl-dir=DIR] [--dump] [--once] [--help]
```

### D2 — `wifi.*` schema + design doc

**Scope.** `modules/wan/wifi/client/schemas/wifi.lua` (auto-loaded
by ds-server) and `docs/design.md`.

Read keys (operator → daemon):

| Key                       | Type    | Default                       | Purpose                                                              |
|---------------------------|---------|-------------------------------|-----------------------------------------------------------------------|
| `wifi.iface`              | string  | `"wlan0"`                     | Kernel iface to manage                                               |
| `wifi.ctrl.dir`           | string  | `"/run/wpa_supplicant"`       | Directory where wpa_supplicant places its control socket             |
| `wifi.wpa.path`           | string  | `"/usr/sbin/wpa_supplicant"`  | Binary path; overridable for tests                                   |
| `wifi.networks`           | string  | `"[]"`                        | JSON array of `{ssid, psk, priority}` (validated in code, not lua)   |
| `wifi.scan.interval.sec`  | integer | 60                            | Periodic background scan cadence; 0 disables                         |
| `wifi.scan.max.results`   | integer | 20                            | Cap published `wifi.scan.results` length                             |
| `wifi.scan.request`       | integer | 0                             | Bump (any change) to trigger an immediate scan                       |
| `wifi.dhcp.client`        | string  | `"auto"`                      | `"udhcpc"` / `"dhclient"` / `"auto"` (probe both)                    |
| `wifi.dhcp.path`          | string  | `""`                          | Override DHCP-client binary path; empty = use default for the choice |

Write keys (daemon → operator):

| Key                       | Type    | Meaning                                                                                      |
|---------------------------|---------|----------------------------------------------------------------------------------------------|
| `wifi.assoc.state`        | string  | `"disconnected"` / `"scanning"` / `"associating"` / `"4way"` / `"connected"` / `"conflict"` / `"exited"` |
| `wifi.assoc.ssid`         | string  | SSID of the AP we're currently associated to; empty when not connected                       |
| `wifi.assoc.bssid`        | string  | BSSID (AP MAC) of the current association                                                    |
| `wifi.signal.rssi`        | integer | Latest signal level (dBm); throttled to 1 write per 5 s                                      |
| `wifi.scan.results`       | string  | JSON array `[{ssid, bssid, signal, flags}, …]`; rewritten on each `CTRL-EVENT-SCAN-RESULTS`  |
| `wifi.scan.last.unix`     | integer | Unix timestamp of the most recent scan that produced a result write                          |
| `wifi.dhcp.state`         | string  | `"idle"` / `"requesting"` / `"bound"` / `"rebinding"` / `"exited"`                            |
| `wifi.dhcp.ip`            | string  | IPv4 leased on the iface (empty when not bound)                                              |
| `wifi.pid.wpa`            | integer | Live wpa_supplicant pid                                                                      |
| `wifi.pid.dhcp`           | integer | Live DHCP-client pid (0 when not running)                                                    |
| `wifi.last.error`         | string  | Last non-fatal error message (auth reject, scan fail, etc.)                                  |

#### `wifi.networks` JSON shape

```json
[
  {"ssid": "HomeAP", "psk": "correcthorse",  "priority": 10},
  {"ssid": "Guest",  "psk": "battery",       "priority": 5},
  {"ssid": "Lab",    "psk": "",              "priority": 1, "key_mgmt": "NONE"}
]
```

Fields: `ssid` (required), `psk` (required unless `key_mgmt="NONE"`),
`priority` (optional, default 0), `key_mgmt` (optional, default
`"WPA-PSK"`; `"NONE"` for open networks). Validation in code via
nlohmann::json; bad shape → `wifi.assoc.state="conflict"` +
`wifi.last.error="bad_networks_json: …"`.

#### `wifi.scan.results` JSON shape

```json
[
  {"ssid": "HomeAP", "bssid": "aa:bb:cc:dd:ee:ff", "signal": -52, "flags": "[WPA2-PSK-CCMP]"},
  …
]
```

Ordered strongest-signal-first, truncated to `wifi.scan.max.results`.

### D3 — DsBridge for `wifi.*`

Mirrors `modules/openvpn/client/src/ds_bridge.{hpp,cpp}` and the
WAN-gate work just merged. Prime + watch all read keys; set the
write keys via `data_store::Client::set`.

Internal cache: latest snapshot of every read key. Setters log
failures via `ACE_ERROR` but never throw — losing a write loses
observability, not safety.

**Tests.** Construct-against-bogus-socket; missing_required spec
(`wifi.iface` is the only required-ish key, defaults to `"wlan0"`,
so missing_required only fires when ds is *unreachable*); on_change
null-callback reset.

### D4 — `wpa_ctrl` over ACE

**Scope.** `modules/wan/wifi/client/src/ctrl.{hpp,cpp}` —
thin ACE wrapper around wpa_supplicant's local control protocol.

Uses `ACE_LSOCK_Connector` + `ACE_LSOCK_Stream` (same primitives
data-store's client already uses). Connects to
`<wifi.ctrl.dir>/<wifi.iface>`. Two channels:

- **Request/reply**: send `CMD\n`, recv up to next prompt. Commands
  used: `PING`, `STATUS`, `SCAN`, `SCAN_RESULTS`, `LIST_NETWORKS`,
  `ADD_NETWORK`, `SET_NETWORK <id> <var> <val>`, `ENABLE_NETWORK`,
  `SELECT_NETWORK`, `DISCONNECT`, `RECONNECT`, `RECONFIGURE`,
  `SIGNAL_POLL`.
- **Unsolicited events**: wpa_supplicant pushes `CTRL-EVENT-*`
  lines on the same socket once we've sent `ATTACH`. Parser splits
  by `\n`, classifies prefix, hands typed `CtrlEvent` to caller.

```cpp
namespace wifi_client::ctrl {

struct CtrlEvent {
    enum class Kind {
        ScanStarted, ScanResults,
        Connected, Disconnected,
        AssocReject, AuthReject,
        Terminating,
        Unknown,
    };
    Kind kind = Kind::Unknown;
    std::string raw;        // full event line for log + debug
    std::string ssid;       // populated for Connected
    std::string bssid;
    std::string reason;     // populated for *Reject / Disconnected
};

class Client {
public:
    bool connect(const std::string& sock_path);
    bool request(const std::string& cmd, std::string& reply);
    std::optional<CtrlEvent> recv_event(int timeout_ms);
    void close();
};

} // namespace wifi_client::ctrl
```

**Tests.** Unit-tests on the parser with canned event traces
(no live wpa_supplicant). End-to-end smoke against a real
wpa_supplicant landed via the `fake-wpa.sh` recorder in D8.

### D5 — process wrapper

**Scope.** `modules/wan/wifi/client/src/process.{hpp,cpp}` — pure
RAII around `ACE_Process` for two child binaries:

- `wpa_supplicant -B is NOT used` — we run it foreground so
  ACE_Process owns the pid: `wpa_supplicant -i <iface>
  -c <generated.conf> -C <ctrl-dir> -D nl80211,wext`.
- `udhcpc -i <iface> -f -q` or `dhclient -d <iface>` — picked at
  D6 from `wifi.dhcp.client`.

The generated wpa_supplicant.conf is built from `wifi.networks`
JSON; same `mkstemp` + `write_temp_config` pattern openvpn-client
uses (`modules/openvpn/client/src/process.cpp`). Lift the
`write_temp_config` helper into a shared header (or copy — TBD at
D5; copy is fine for v1).

```
ctrl_interface=DIR=<ctrl-dir>
ctrl_interface_group=0
update_config=0

network={
    ssid="HomeAP"
    psk="correcthorse"
    priority=10
}
…
```

**Tests.** /bin/sh stand-ins like the openvpn process tests:
spawn/exit/terminate/destructor-reaps.

### D6 — lifecycle FSM + supervisor

**Scope.** `modules/wan/wifi/client/src/lifecycle.{hpp,cpp}` (pure
FSM) and `supervisor.{hpp,cpp}` (impure wiring) — same shape as the
WAN-gate Supervisor.

#### Lifecycle (pure)

Input: `ctrl::CtrlEvent`. Output: callbacks (`Sinks`) that the
supervisor wires to DsBridge setters.

States:

```
   disconnected  ◀── start
        │
        ▼
    scanning  ◀── CTRL-EVENT-SCAN-STARTED
        │
        ▼ (results pushed to wifi.scan.results)
   associating  ◀── chosen SSID → SELECT_NETWORK
        │
        ▼
       4way    ◀── CTRL-EVENT-CONNECTED is delayed until handshake
        │
        ▼
   connected  ◀── then supervisor spawns DHCP child
        │
        ▼ (events: Disconnected, Terminating)
   disconnected
```

Pure tests with canned event sequences (mirror
`modules/openvpn/client/test/lifecycle_test.cpp`).

#### Supervisor (impure)

Owns:

- `DsBridge& m_ds`
- `ctrl::Client m_ctrl`
- `OpenVpnProcess`-style `Process m_wpa, m_dhcp`
- `Lifecycle m_fsm`

Outer loop drives:

1. Generate wpa_supplicant.conf from current `wifi.networks`.
2. Spawn wpa_supplicant → wait for control socket → `ATTACH`.
3. Issue initial `SCAN`.
4. Event loop: `recv_event` with 200 ms timeout (matches the
   openvpn-client supervisor's polling cadence so the WAN-gate
   pattern carries over wholesale).
5. On `Connected` event: spawn DHCP child; publish
   `wifi.dhcp.state="requesting"`.
6. On `Disconnected`/`Terminating`: reap DHCP, publish
   `wifi.dhcp.state="exited"`; drop back to `scanning`.
7. On `wifi.networks` change (from DsBridge on_change): if
   additive → `RECONFIGURE`; otherwise full restart.
8. On `wifi.scan.request` flip → issue `SCAN`.

Same shutdown story openvpn-client has: SIGTERM kills the daemon,
the `OpenVpnProcess` destructor reaps both children, no zombies.

### D7 — packaging + systemd

**Scope.**

- `packaging/Containerfile` — `apt install -y wpasupplicant udhcpc
  wireless-tools`. Mirrors the openvpn install step.
- `packaging/systemd/iot-wifi-client.service`:

  ```
  [Service]
  Type=exec
  ExecStart=/usr/bin/wifi-client --ds-sock=/run/iot/data_store.sock
  RuntimeDirectory=iot
  DynamicUser=yes
  AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW
  DeviceAllow=/dev/rfkill rw
  After=iot-ds.service
  Wants=iot-ds.service
  Restart=on-failure
  ```

- `packaging/iot-entrypoint.sh` — `IOT_ROLE=wifi` branch.
- `docker/Dockerfile` (dev image) — same apt installs.
- `DEPLOY.md` — operator section for wifi-client, both container
  and bare-metal paths, mirrors L13's net-router additions.

### D8 — `log/L15/smoke.sh`

**Scope.** Manual e2e smoke. Same shape as L14's compose harness.

1. Build the iot image if absent (or rely on the L14 compose flow
   if L14 lands first).
2. Bring up `ds-server` + `wifi-client` with a `fake-wpa.sh` that
   stands in for wpa_supplicant — accepts the control protocol
   over a unix socket, emits canned CTRL-EVENT lines, replies to
   `SCAN_RESULTS` with a fixed list.
3. Seed `wifi.networks` via ds-cli with the SSID the fake serves.
4. Assert `wifi.assoc.state` reaches `connected`.
5. Assert `wifi.scan.results` is non-empty JSON.
6. (Optional once net-router is in the same compose) Assert
   `net.iface.active="wlan0"`.

Real wpa_supplicant + a real radio is out of scope — same
rationale L14 used for openvpn (no AP to test against, rootless
podman can't talk to the host's wlan stack).

---

## 3. Acceptance for L15

L15 closes when:

- `bash log/L15/smoke.sh` exits 0 on a clean machine with podman
  installed.
- `wifi.*` schema is enforced by ds-server (set with bad type →
  `SchemaRejected`).
- Eight D-PRs merged (D1 plan + D2 schema/design + D3 DsBridge
  + D4 ctrl + D5 process + D6 lifecycle+supervisor + D7 packaging
  + D8 smoke).
- `modules/wan/wifi/client/docs/design.md` documents the
  state machine, the NetworkManager conflict-detection story, and
  the plaintext-PSK posture.
- DEPLOY.md shows the wifi-client invocation alongside the
  existing ds / lwm2m / openvpn / net-router daemons.

---

## 4. After L15

Candidate phases (not committed):

- **L16a — secrets vault for `wifi.networks` + `vpn.*`.** Encrypt
  at rest in ds-server; decrypt on demand for the consuming daemon.
  Removes plaintext PSK + cert/key paths from the data store.
- **L16b — Object 4 (Connectivity Monitoring).** Wire
  `wifi.signal.rssi` + `net.iface.active` into the lwm2m stack so
  a server can read the device's current bearer + signal.
- **L16c — cellular daemon at `modules/wan/cellular/`.** Same shape
  as wifi-client but for the WWAN modem (ModemManager / mmcli or
  raw AT-over-serial — decision at L16c/D1).
- **L16d — wifi AP mode at `modules/wan/wifi/ap/`.** The placeholder
  path reserved here. Different state machine (hostapd not
  wpa_supplicant); shares the cmake layout convention.
- **L16e — NetworkManager coexistence.** Cooperative mode: detect
  NM, hand off iface management via D-Bus instead of refusing.
- **L16f — chaos coverage (was L15a in the L14 plan).** Kill each
  daemon mid-flight, prove the others degrade cleanly. Now that
  wifi-client + net-router + openvpn-client all sit on the same
  WAN-gate chain, this gets concrete.

Pick one when L15 is in.
