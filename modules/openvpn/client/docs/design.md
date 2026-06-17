# openvpn-client — module design (L12)

> **Status (2026-06-01):** L12 closed. D1+D2 (scaffold), D3 (DsBridge),
> D4 (mgmt parser), D5 (ACE_Process wrapper), D6 (lifecycle + e2e
> smoke) all done. L15 follow-on adds the WAN gate (subscribes to
> `net.iface.active`, only spawns openvpn while a WAN iface is up).

## What this module is

A daemon that owns the lifecycle of an `openvpn(8)` subprocess and
mirrors its state into the data store. Operators configure via
`ds-cli set vpn.*`; the daemon spawns openvpn with a generated
config, watches its management interface, and publishes the assigned
virtual IP + state back to the same store.

## Runtime requirements

| Dep                       | Why                                                                          | Where it's installed                                |
|---------------------------|------------------------------------------------------------------------------|------------------------------------------------------|
| `openvpn(8)`              | Spawned by `OpenVpnProcess`; default path `/usr/sbin/openvpn`                | `docker/Dockerfile` (dev) + `packaging/Containerfile` (OCI runtime) |
| `CAP_NET_ADMIN` capability | openvpn writes routes + brings the TUN device up                              | Container: `--cap-add=NET_ADMIN`. Bare-metal systemd: needs `AmbientCapabilities=CAP_NET_ADMIN` (FUP for the systemd unit) |
| `/dev/net/tun`            | The TUN device node openvpn opens                                            | Container: `--device=/dev/net/tun`. Bare-metal: already there |
| `ds-server` reachable     | DsBridge connects to the unix socket on startup                              | Same `/run/iot/data_store.sock` the lwm2m binary uses |

Container example:

```sh
podman run -d --name iot-ovpn \
    --cap-add=NET_ADMIN \
    --device=/dev/net/tun \
    --volumes-from iot-ds \
    -v /etc/iot/vpn:/etc/iot/vpn:ro \
    iot:l11 openvpn-client --ds-sock=/run/iot/data_store.sock
```

```
┌──────────────────────────────────────────────────────────────┐
│                       Operator / Other apps                  │
│        ds-cli set vpn.*       ds-cli get vpn.assigned.ip     │
└──────────────────────┬───────────────────────────────────────┘
                       │ EMP over /run/iot/data_store.sock
                  ┌────▼─────┐
                  │ ds-server│  (schemas: iot.lua, vpn.lua)
                  └────┬─────┘
                       │ libdatastore_client (watch + get + set)
                  ┌────▼──────────┐
                  │ openvpn-client│  this module
                  │  (ACE_Reactor)│
                  └───┬───────┬───┘
        ACE_Process   │       │  ACE_SOCK_Stream
                      │       └──────────┐
                      ▼                  ▼
                ┌──────────┐       ┌──────────────────┐
                │ openvpn  │◀──────│ mgmt 127.0.0.1   │
                │ binary   │       │   :<vpn.mgmt.port│
                └────┬─────┘       └──────────────────┘
                     │ TUN device
                     ▼
                  remote VPN
```

## Who does what on the kernel

A common question: **who actually creates `tun0`, sets the IP, and
installs the routes when a `PUSH_REPLY` arrives?**

Answer: **openvpn(8), not our daemon.** This module never touches
the kernel network state — it observes and publishes. The split:

| Step                                             | Owner                | How                                              |
|--------------------------------------------------|----------------------|--------------------------------------------------|
| Open `/dev/net/tun`, allocate `tun0`            | openvpn(8) subprocess | `ioctl(TUNSETIFF)`                               |
| Set IP `10.8.0.6` on `tun0`                     | openvpn(8)            | `ioctl(SIOCSIFADDR)` / netlink                   |
| Set netmask `255.255.255.0`                     | openvpn(8)            | `ioctl(SIOCSIFNETMASK)`                          |
| Bring interface up, install routes              | openvpn(8)            | `ioctl(SIOCSIFFLAGS)` + `RTM_NEWROUTE`           |
| Observe what happened + publish to ds-server     | this module           | mgmt `>PUSH_REPLY:` + `>STATE:` → `DsBridge::set_assigned_*` |

That's why the systemd unit ships `AmbientCapabilities=CAP_NET_ADMIN`
+ `DeviceAllow=/dev/net/tun rw` — those are openvpn(8)'s requirements,
not ours. Our daemon could run as nobody; openvpn can't.

Sequence on a fresh PUSH_REPLY:

1. openvpn-server pushes `ifconfig 10.8.0.6 255.255.255.0,route-gateway 10.8.0.1,…`
2. openvpn(8) **applies it to the kernel** (creates tun0, sets IP,
   installs routes) — this is non-negotiable in the default config.
3. openvpn(8) emits management lines: `>STATE:…,ASSIGN_IP,…,10.8.0.6,…`,
   then `>PUSH_REPLY:…`, then `>STATE:…,CONNECTED,…`.
4. Our `Lifecycle::step` parses + DsBridge writes
   `vpn.assigned.{ip,gateway,netmask,dns}` to ds-server.
5. Other apps observe via `ds-cli get vpn.assigned.ip` or
   `Client::watch`.

**The data store is a mirror of what openvpn already did to the
kernel**, not the source of truth. An app reading `vpn.assigned.ip`
out of ds-server can assume the tunnel exists with that address; if
they want to re-verify, they can `ip addr show tun0`.

### What if we wanted to own the kernel state ourselves?

openvpn(8) accepts `--ifconfig-noexec --route-noexec` plus an
`--up <script>` callback to suppress its default kernel writes and
hand the assigned values to an external program. The external
program would then issue the netlink commands itself — much bigger
surface (privilege, error handling, route table conflicts) and
moves us into the same problem space NetworkManager / systemd-networkd
already solve. L12's scope explicitly took the smaller bet: let
openvpn do the kernel work, we just watch.

## WAN gate (L15)

openvpn(8) won't reach the server if there's no usable WAN. Rather
than letting it spin in `RECONNECTING` until something happens, the
daemon **subscribes to `net.iface.active`** (the highest-priority
OPER UP iface that net-router has selected: eth0 / wlan0 / wwan0)
and only spawns openvpn while that key is non-empty.

State machine:

| `net.iface.active`       | bound  | action               | `vpn.gate.reason` |
|--------------------------|--------|----------------------|-------------------|
| unset / empty            | none   | stay idle            | `wan_down`        |
| unset / empty            | eth0   | terminate child      | `wan_down`        |
| eth0                     | none   | spawn (bind → eth0)  | `ok`              |
| eth0                     | eth0   | no-op                | `ok`              |
| wlan0 (was eth0)         | eth0   | terminate + respawn  | `ok`              |

The watch callback runs on the data_store::Client listener thread;
it sets a "dirty" flag + notifies a condition variable. The mgmt
event loop's existing 200 ms `recv` timeout doubles as the polling
interval for the flag — on WAN change, the loop exits within one
poll, the child is SIGTERM'd, and the supervisor's outer loop
respawns bound to the new iface.

**Hard dependency on net-router.** With no net-router running,
`net.iface.active` is never set and openvpn-client never spawns.
That's intentional for the L13+ product shape; dev/standalone
runs can short-circuit by setting the key manually:

```sh
ds-cli set net.iface.active '"eth0"'
```

Two write keys back to the operator:
- `vpn.gate.reason` — `"ok"` while running, `"wan_down"` while
  gated, `"spawn_failed"` if the last spawn attempt failed.
- `vpn.bound.iface` — which WAN iface the current session is
  bound to (empty when idle).

The Gate FSM itself is a pure C++ class (`src/gate.{hpp,cpp}`) —
no I/O, no threading; fully unit-tested in `test/gate_test.cpp`.
The Supervisor (`src/supervisor.{hpp,cpp}`) wires it to DsBridge +
OpenVpnProcess.

## Enable gate + cert-arrival respawn (L16/D4)

Alongside the WAN gate, the Supervisor watches
`services.openvpn.client.enable` via `data_store::ServiceGate`
(`src/supervisor.cpp` — L16/D4). Setting it `false` tears the child
down within one event-loop tick (NFR-SVC-001); setting it `true`
lets the outer loop respawn openvpn once WAN + deps are healthy.

This same enable gate is how a **cloud-pushed VPN certificate** takes
effect on the RPi/systemd image, which ships **no cert sidecar**
watching `/etc/iot/vpn`:

1. The cloud pushes the cert family over LwM2M custom **Object 2048**
   (instances 0/1/2 = ca/cert/key) and EXECUTEs RID 3 (Apply).
2. On the device, `install_cert` (`apps/src/lwm2m_object_stubs.cpp`)
   materialises `/etc/iot/vpn/{ca.crt,client.crt,client.key}`, then
   calls its `CertHooks::apply`. The image **must** ship `/etc/iot/vpn`
   (created by tmpfiles, `2750 engineer:iot`) — the lwm2m client runs as
   `engineer` and can't `mkdir` under root-owned `/etc/iot`, so without
   it the write fails `ENOENT` and no certs land. The key is written
   `0640` (group `iot`) so the `openvpn-client` DynamicUser
   (`SupplementaryGroups=iot`) can read it; `ca.crt`/`client.crt` are
   `0644`.
3. The client wiring (`apps/src/main.cpp`) supplies that hook as a
   **ds gate-flip**: it `set`s `services.openvpn.client.enable`
   `false` then `true`. ds watches fire on *change*, so the deliberate
   `false→true` edge is what guarantees the gate sees a transition (a
   bare `true` write over an already-`true` value emits no event).
4. The gate-flip wakes this Supervisor: openvpn is (re)spawned and
   reads the just-written cert family at exec time — picking up the
   new credentials with no sidecar and no manual restart.

For the primary flow (first-time provisioning — openvpn was down or
crash-looping because the cert was absent), even a single coalesced
`true` edge re-spawns the idle supervisor, so the cert is picked up.
A *rotation* while the tunnel is already connected relies on the
`false` edge being delivered to tear the live child down first; if
the data-store coalesces the two rapid writes to the final `true`
only, an already-connected session keeps the old cert until its next
natural respawn (WAN/dep/enable event). Initial provisioning — the
case that lets the device connect at all — is unaffected.

## Why ACE (not libevent like grace-server)

The rest of this repo (lwm2m, ds-server, ds-cli) all run on
`ACE_Reactor` + `ACE_Task`. Using ACE here keeps the threading
story uniform and reuses primitives the codebase already has
under test:

- `ACE_Reactor` — one event loop per process
- `ACE_Event_Handler` — wraps the mgmt socket fd
- `ACE_SOCK_Stream` + `ACE_INET_Addr` — connects to `localhost:<mgmt-port>`
- `ACE_Process` + `ACE_Process_Manager` — fork/exec openvpn, wait on SIGCHLD

## Schema layout

`schemas/vpn.lua` (installed to `/etc/iot/ds-schemas/vpn.lua` by the
module's cmake install rule) declares every readable + writable key
with type + range constraints. ds-server validates at set time —
`ds-cli set vpn.remote.port 99999` is rejected as `SchemaRejected`
before the daemon ever sees the value. Full key list in the schema
file itself; quick reference in [L12 plan §2 D1](../../../log/L12/plan.md).

## Module layout

```
modules/openvpn/client/
├── CMakeLists.txt
├── inc/
│   └── client.hpp            v0 public API (#include "client.hpp")
├── src/
│   ├── main.cpp              CLI parse + entry
│   ├── main_impl.cpp         v0 dump-vpn-keys + run_daemon wrapper
│   ├── ds_bridge.{hpp,cpp}   data-store I/O (vpn.* + net.iface.active)
│   ├── mgmt_protocol.{hpp,cpp}  openvpn mgmt-interface parser
│   ├── process.{hpp,cpp}     ACE_Process wrapper
│   ├── lifecycle.{hpp,cpp}   mgmt-event → vpn.* sink FSM (D6)
│   ├── gate.{hpp,cpp}        pure WAN-gate FSM (L15)
│   └── supervisor.{hpp,cpp}  gated spawn/serve outer loop (L15)
├── schemas/vpn.lua
├── test/                     gtest suite
└── docs/design.md            this file
```

## State machine (D6)

Same `vpn.state` values the schema declares. Wire shape:

```
            ┌──────────────┐
            │ disconnected │   ◀── start state
            └──────┬───────┘
                   │ (spawn openvpn)
            ┌──────▼───────┐
            │ resolving    │   ◀── >STATE:RESOLVE
            └──────┬───────┘
                   │
            ┌──────▼───────┐
            │ connecting   │   ◀── >STATE:TCP_CONNECT / WAIT
            └──────┬───────┘
                   │
            ┌──────▼───────┐
            │ auth         │   ◀── >STATE:AUTH
            └──────┬───────┘
                   │
            ┌──────▼───────┐
            │ wait         │   ◀── >STATE:GET_CONFIG
            └──────┬───────┘
                   │
            ┌──────▼───────┐
            │ connected    │   ◀── >STATE:CONNECTED  +  >PUSH_REPLY:…
            └──────┬───────┘  (we write vpn.assigned.* here)
                   │ (SIGCHLD or SIGINT)
            ┌──────▼───────┐
            │ exited       │   ◀── ACE_Process::wait() returns
            └──────────────┘
```

First-cut (L12/D6) quiesces at `connected`. The L15 WAN gate adds
two more transitions out of every state above:

- `net.iface.active` cleared → SIGTERM child → `disconnected`
  (`vpn.gate.reason=wan_down`).
- `net.iface.active` changes to a different iface → SIGTERM child
  → re-enter `disconnected` → re-spawn bound to the new iface
  (`vpn.bound.iface` updates atomically).

Auto-restart on bare child crash (no WAN event) is still FUP-L12-1.

## Known issue: `vpn.state` stuck at `connecting`

> **Status: known issue, fix pending.** The tunnel itself works.

**Symptom.** The tunnel is fully up — `tun0` has the assigned IP,
openvpn logged `Initialization Sequence Completed`, and the cloud's
`cloud.vpn.connected` lists the endpoint — yet `vpn.state` reads
`connecting` and never advances to `connected`. `vpn.assigned.ip` is still
set correctly.

**Cause.** On attach the supervisor sends `state on` (`supervisor.cpp`),
which enables *future* real-time `>STATE:` notifications. It does **not**
issue a query for the *current* state. If openvpn already reached
`CONNECTED` before the daemon attached the mgmt socket, that transition
was emitted before notifications were on and is therefore missed — so the
Lifecycle never sees the `CONNECTED` `>STATE:` event that would write
`vpn.state=connected`. `vpn.assigned.ip` still gets set because the
`>PUSH_REPLY` line is parsed independently.

The mgmt parser (`mgmt_protocol.cpp`) only recognises the prefixed
`>STATE:` / `>PUSH_REPLY:` events. A bare `state` *query response* (the
state record without the `>STATE:` prefix) is classified as a `DataLine`
response payload and ignored by the Lifecycle — so even if the current
state were requested today, the reply wouldn't update `vpn.state`.

> Note: the inline comment at `supervisor.cpp` (`state on` "makes it emit
> the CURRENT state immediately") describes the *intended* behavior; in
> practice the current-state emission arrives as the bare state-record
> response that the parser drops, which is why this issue persists.

**Pending fix.** After `state on`, also send `state 1` (query the current
state) **and** teach the parser to surface the bare state-record response
as a `State` event so the Lifecycle promotes `vpn.state` to `connected`
even when the daemon attached late.

## Related docs

- [L12 plan](../../../log/L12/plan.md) — phase plan + risk register
- [data-store protocol](../../data-store/docs/protocol.md) — EMP framing
- [data-store client API](../../data-store/docs/client_api.md) — the lib we link
- [apps/src/ds_config.cpp](../../../apps/src/ds_config.cpp) — DsBridge's template
