# openvpn-client — module design (L12)

> **Status (2026-05-31):** L12 closed. D1+D2 (scaffold), D3 (DsBridge),
> D4 (mgmt parser), D5 (ACE_Process wrapper), D6 (lifecycle + e2e
> smoke) all done; openvpn(8) install + cap docs in this PR.

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
│   ├── main_impl.cpp         v0 dump-vpn-keys (D2)
│   ├── ds_bridge.{hpp,cpp}   (D3 — pending)
│   ├── mgmt_protocol.{hpp,cpp}  (D4 — pending)
│   ├── process.{hpp,cpp}     (D5 — pending)
│   └── client.cpp            lifecycle FSM (D6 — pending)
├── schemas/vpn.lua
├── test/                     gtest suite (D3 onward)
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

First-cut (L12/D6) quiesces at `connected`. Reconnect is FUP-L12-1.

## Related docs

- [L12 plan](../../../log/L12/plan.md) — phase plan + risk register
- [data-store protocol](../../data-store/docs/protocol.md) — EMP framing
- [data-store client API](../../data-store/docs/client_api.md) — the lib we link
- [apps/src/ds_config.cpp](../../../apps/src/ds_config.cpp) — DsBridge's template
