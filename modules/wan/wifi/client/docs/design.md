# wifi-client — module design (L15)

> Status: **L15/D1+D2 landed**, D3..D8 in flight per
> [`log/L15/plan.md`](../../../../log/L15/plan.md) and the TDD
> traceability at [`log/L15/tdd.md`](../../../../log/L15/tdd.md).

## What this module is

`wifi-client` is a single-host daemon that owns the
`wpa_supplicant(8)` lifecycle for one wifi interface, drives scan
+ association via wpa_supplicant's local control protocol, and
spawns a sibling DHCP child once associated. It publishes scan
results, signal level, association state, and DHCP state into the
data store under the `wifi.*` namespace.

Slot in the WAN-gate chain: operator sets `wifi.networks` →
wifi-client brings `wlan0` up → kernel writes OPER UP → net-router
sees it → publishes `net.iface.active="wlan0"` → openvpn-client's
WAN gate fires → tunnel comes up. wifi-client is one link in that
chain; it does not know about openvpn or net-router.

## Runtime requirements

| Dep                | Why                                                              | Where installed                          |
|--------------------|------------------------------------------------------------------|------------------------------------------|
| `wpa_supplicant`   | Spawned to do the radio/auth/4-way-handshake work                | `/usr/sbin/wpa_supplicant` (apt: `wpasupplicant`) |
| `udhcpc`           | Spawned to lease an IPv4 once associated (default DHCP client)   | `/usr/bin/udhcpc` (apt: `udhcpc` from busybox)    |
| `dhclient`         | Fallback DHCP client when `wifi.dhcp.client="dhclient"`          | `/usr/sbin/dhclient` (apt: `isc-dhcp-client`)     |
| `CAP_NET_ADMIN`    | wpa_supplicant configures iface state + manages keys             | systemd `AmbientCapabilities=`           |
| `CAP_NET_RAW`      | Scan requires raw sockets                                        | systemd `AmbientCapabilities=`           |
| `/run/wpa_supplicant` | Control socket parent dir (mkdir -p on startup)               | `RuntimeDirectory=` in the systemd unit  |
| `/dev/rfkill`      | wpa_supplicant queries soft-block state                          | systemd `DeviceAllow=/dev/rfkill rw`     |

D7 lands the systemd unit + the apt installs in
`packaging/Containerfile` + `docker/Dockerfile`.

## Architecture (D3..D6)

```
            ┌────────────────────────────────────────┐
            │              wifi-client                │
            │                                         │
   ds-cli   │  ┌──────────┐    ┌──────────────────┐  │
   set …  ──┼─▶│ DsBridge │───▶│   Supervisor     │  │
            │  │  (D3)    │    │    (D6)          │  │
            │  └──────────┘    │  Lifecycle FSM   │  │
            │       ▲           │  pure (testable) │  │
            │       │           └────────┬─────────┘  │
            │ wifi.assoc.state            │            │
            │ wifi.signal.rssi            │            │
            │ wifi.scan.results          ▼            │
            │       │           ┌──────────────────┐  │
            │       └───────────│  ctrl::Client    │  │
            │                   │    (D4)          │  │
            │                   │  ACE LSOCK over  │  │
            │                   │ /run/wpa_supp/<i>│  │
            │                   └────────┬─────────┘  │
            │                            │            │
            │                   ┌────────▼─────────┐  │
            │                   │  Process (D5)    │  │
            │                   │  RAII ACE_Process│  │
            │                   └────────┬─────────┘  │
            └────────────────────────────┼────────────┘
                                         │ fork/exec
                                         ▼
                          ┌─────────────────────────────┐
                          │  wpa_supplicant(8) -i <if>  │
                          │  udhcpc -i <if> -f -q       │
                          └─────────────────────────────┘
```

`DsBridge` and `Supervisor` are the only pieces that touch the
data store; `ctrl::Client` only knows the wpa_supplicant control
protocol; `Process` only knows fork/exec/reap. This mirrors
openvpn-client's split exactly so a reader who's seen one module
recognises the layout.

## Who does what on the kernel

- **wifi-client (this daemon)**: owns the *policy* — which SSID to
  associate to, when to scan, which DHCP client to spawn. It
  configures wpa_supplicant via the control socket and observes
  the kernel iface state by reading wpa_supplicant's CTRL-EVENT
  stream. It does NOT directly `ioctl()` netlink or `ip link set`.
- **wpa_supplicant**: owns the *mechanism* — does the scan, the
  authentication exchange, the 4-way handshake, the iface keys.
  Brings the iface to L2-ready. Writes its CTRL-EVENT stream over
  the unix control socket.
- **udhcpc/dhclient**: owns the L3 lease. wifi-client spawns it
  on `CTRL-EVENT-CONNECTED`, reaps it on `CTRL-EVENT-DISCONNECTED`.
- **kernel**: owns the actual netdev. On association + DHCP, the
  iface goes OPER UP, which net-router observes independently via
  its iface_monitor (`net.iface.active`).

### What if we wanted to own the kernel state ourselves?

Same trade openvpn-client made: re-implementing wpa_supplicant's
scan/auth/4-way state machine in C++ is a multi-quarter project
with security implications. We delegate to upstream; the daemon
is a thin policy layer that publishes observable state.

## NetworkManager conflict gate

REQ-WIFI-022: on startup the daemon probes for a competing
manager and refuses to spawn wpa_supplicant if it finds one. Two
checks:

1. `systemctl is-active NetworkManager` returns `active` → conflict.
2. `<wifi.ctrl.dir>/<wifi.iface>` already exists as a socket →
   somebody else is talking to wpa_supplicant.

On either positive, the daemon writes `wifi.assoc.state="conflict"`
+ `wifi.last.error="…"` and stays parked. Operator fixes the
collision (typically `systemctl mask NetworkManager`) and
re-enables via a restart.

Cooperative coexistence (handing iface management to NM via D-Bus
on the fly) is FUP-L16e — out of L15 scope.

## Plaintext PSK posture

`wifi.networks[].psk` is stored in ds-server's `data_store.lua` in
plaintext, same posture L12 took for `vpn.cert.path` / `vpn.ca.path`
(they're plain paths, the cert content is on disk in cleartext).
Operator with the ds-server socket can read every PSK; that's the
threat model.

A secrets-vault store (encrypt at rest in ds-server, decrypt on
demand by the consuming daemon) is FUP-L15-1 → L16a. Documented
non-goal here, not a defect.

## Schema layout

[`schemas/wifi.lua`](../schemas/wifi.lua) declares every read +
write key with `(type, default, min?, max?)`. ds-server
auto-loads it from `/etc/iot/ds-schemas/` so a set with a bad
type lands as `SchemaRejected` on the wire.

Read keys (operator → daemon): `wifi.iface`, `wifi.ctrl.dir`,
`wifi.wpa.path`, `wifi.networks`, `wifi.scan.interval.sec`,
`wifi.scan.max.results`, `wifi.scan.request`, `wifi.dhcp.client`,
`wifi.dhcp.path`.

Write keys (daemon → operator): `wifi.assoc.state`, `wifi.assoc.ssid`,
`wifi.assoc.bssid`, `wifi.signal.rssi`, `wifi.scan.results`,
`wifi.scan.last.unix`, `wifi.dhcp.state`, `wifi.dhcp.ip`,
`wifi.pid.wpa`, `wifi.pid.dhcp`, `wifi.last.error`.

### `wifi.networks` JSON shape

```json
[
  {"ssid": "HomeAP", "psk": "correcthorse",  "priority": 10},
  {"ssid": "Guest",  "psk": "battery",       "priority": 5},
  {"ssid": "Lab",    "psk": "",              "priority": 1, "key_mgmt": "NONE"}
]
```

Fields:
- `ssid` (required, string)
- `psk` (required unless `key_mgmt="NONE"`, string)
- `priority` (optional, integer, default 0)
- `key_mgmt` (optional, default `"WPA-PSK"`; `"NONE"` for open
  networks)

Validated in code via nlohmann::json (D5). Bad shape →
`wifi.assoc.state="conflict"` + `wifi.last.error="bad_networks_json: …"`.

### `wifi.scan.results` JSON shape

```json
[
  {"ssid": "HomeAP", "bssid": "aa:bb:cc:dd:ee:ff", "signal": -52, "flags": "[WPA2-PSK-CCMP]"},
  {"ssid": "Guest",  "bssid": "aa:bb:cc:dd:ee:00", "signal": -67, "flags": "[WPA2-PSK-CCMP]"}
]
```

Ordered strongest-signal-first, truncated to
`wifi.scan.max.results` (default 20). Rewritten on each
`CTRL-EVENT-SCAN-RESULTS`; no rolling-window history (NFR-WIFI-003).

## Module layout

```
modules/wan/wifi/client/
├── CMakeLists.txt                  # wifi_client_lib + wifi-client + tests
├── inc/
│   └── client.hpp                  # v0 entry points + ParsedCli
├── src/
│   ├── main.cpp                    # CLI dispatch (thin)
│   ├── main_impl.cpp               # parse_cli + v0_dump + run_daemon stub
│   ├── ds_bridge.{hpp,cpp}         # D3 — wifi.* prime + watch + setters
│   ├── ctrl.{hpp,cpp}              # D4 — ACE LSOCK + event parser
│   ├── process.{hpp,cpp}           # D5 — RAII over ACE_Process
│   ├── lifecycle.{hpp,cpp}         # D6 — pure FSM
│   └── supervisor.{hpp,cpp}        # D6 — impure wiring
├── schemas/
│   └── wifi.lua                    # auto-loaded by ds-server
├── test/                           # gtest, one combined binary
│   ├── main_test.cpp               # D1 — parse_cli + v0_dump
│   ├── ds_bridge_test.cpp          # D3
│   ├── ctrl_test.cpp               # D4
│   ├── process_test.cpp            # D5
│   ├── lifecycle_test.cpp          # D6
│   └── supervisor_test.cpp         # D6
└── docs/
    └── design.md                   # this file
```

## State machine (D6 preview)

```
   ┌──────────────┐
   │ disconnected │◀────────────────┐
   └──────┬───────┘                 │
          │ wpa.start  + SCAN       │
          ▼                         │
   ┌──────────────┐                 │
   │   scanning   │                 │
   └──────┬───────┘                 │
          │ CTRL-EVENT-SCAN-RESULTS │
          │ → choose best SSID      │
          │ → SELECT_NETWORK <id>   │
          ▼                         │
   ┌──────────────┐                 │
   │ associating  │                 │
   └──────┬───────┘                 │
          │ associated (key mgmt)   │
          ▼                         │
   ┌──────────────┐                 │
   │     4way     │                 │
   └──────┬───────┘                 │
          │ CTRL-EVENT-CONNECTED    │
          │ → spawn DHCP child      │
          ▼                         │
   ┌──────────────┐                 │
   │  connected   │                 │
   └──────┬───────┘                 │
          │ CTRL-EVENT-DISCONNECTED │
          │ / -TERMINATING / -REJECT│
          └─────────────────────────┘
```

`conflict` and `exited` are terminal-ish states reached from any
of the above:
- `conflict`: NM-conflict gate (REQ-WIFI-022); only restart escapes.
- `exited`: wpa_supplicant subprocess exited unexpectedly; the
  supervisor publishes the state and respawns on its retry
  schedule (D6 design — mirrors openvpn's `vpn.state="exited"`
  + retry path).

## Module layout convention

`modules/wan/` is the parent for *radio-link / WAN access-layer*
daemons. Subdirs:

- `wifi/client/` — this module (wpa_supplicant supplicant role)
- `wifi/ap/` — reserved for hostapd-based AP mode (FUP-L16d)
- `cellular/` — reserved for ModemManager / mmcli or raw AT-over-serial (FUP-L16c)

Single-radio per daemon by convention. A second radio means a
second daemon instance keyed by `--iface=` against a second
control-dir, or schema generalisation (FUP).

## Related docs

- [`../../../../log/L15/plan.md`](../../../../log/L15/plan.md) — L15 forward plan
- [`../../../../log/L15/tdd.md`](../../../../log/L15/tdd.md) — REQ/NFR traceability
- [`../schemas/wifi.lua`](../schemas/wifi.lua) — authoritative schema
- [`../../../openvpn/client/docs/design.md`](../../../openvpn/client/docs/design.md) — sibling module; this design mirrors its structure
- [`../../../net/router/docs/design.md`](../../../net/router/docs/design.md) — downstream consumer (sees `wlan0` OPER UP)
