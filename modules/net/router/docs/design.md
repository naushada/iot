# net-router — module design (L13)

> **Status (2026-05-31):** L13 complete (D1–D7). The daemon wires
> DsBridge + iface_monitor + ip_route + apply + Lifecycle behind a
> simple poll loop, ships as `iot-net-router.service` (systemd) and
> `IOT_ROLE=net` (container). nftables + iproute2 are now in both
> the dev image and the runtime image.

## What this module is

A daemon that watches `vpn.assigned.*` + `iot.*` via ds-server,
generates an **nftables** ruleset that DNATs inbound tunnel traffic
to the lwm2m client, and manages outgoing-traffic interface
priority (eth → wifi → cellular) via `ip route` metric writes.
Operator-set custom forward/drop/accept rules flow through a single
`net.custom_rules` ds-server key as a JSON-encoded string.

```
              ┌──────────────────────────────────────┐
              │  Operator / Other apps               │
              │  ds-cli set net.lwm2m.target_ip ...  │
              │  ds-cli set net.custom_rules ...     │
              └─────────────────┬────────────────────┘
                                │ EMP over /run/iot/data_store.sock
                          ┌─────▼─────┐
                          │ ds-server │  (schemas: iot.lua, vpn.lua, net.lua)
                          └─────┬─────┘
                                │ libdatastore_client (watch + get + set)
                        ┌───────▼──────────┐
                        │ net-router       │  this module
                        │ (ACE_Reactor)    │
                        └───┬───────┬──────┘
              shell-out     │       │  shell-out
                   ┌────────▼──┐  ┌─▼──────────┐
                   │ nft -f -  │  │ ip route   │
                   │ (kernel)  │  │ replace ...│
                   └───────────┘  └────────────┘
```

## Runtime requirements

| Dep                       | Why                                                                  | Where it's installed                                  |
|---------------------------|----------------------------------------------------------------------|--------------------------------------------------------|
| `nft` (nftables userspace) | Pipes the generated ruleset to `nft -f -` for atomic apply           | docker/Dockerfile (dev) + packaging/Containerfile (D7) |
| `ip` (iproute2)            | `ip link show -j` for iface monitoring; `ip route replace` for prio  | already in base ubuntu                                 |
| `nft_*` kernel modules     | `nft_nat`, `nft_chain_nat`, `nft_chain_route`                        | host kernel — distro defaults are fine                 |
| `CAP_NET_ADMIN`            | Required for nftables apply AND `ip route` table writes              | systemd unit (D7) ships `AmbientCapabilities=CAP_NET_ADMIN` |
| ds-server reachable        | DsBridge connects on startup                                         | same `/run/iot/data_store.sock` as the other daemons   |

## Why nftables (not iptables)

User explicitly picked the modern path. Wins:
- Atomic ruleset apply via `nft -f -` (single transaction)
- Cleaner data model: tables/chains/rules with explicit hooks + priorities
- Single binary handles both v4 + v6 via `inet` family

Trade-off: minimal hosts may not ship `nft`. D7 apt-installs it.
An iptables fallback is FUP only if a target without nftables surfaces.

## Schema layout

`schemas/net.lua` (installed to `/etc/iot/ds-schemas/net.lua` by D7's
install rule). 9 read keys + 6 write keys; the only required read
key is `net.lwm2m.target_ip`. Custom rules ship as a JSON-encoded
string in `net.custom_rules`; shape is validated at the JSON-parse
step in the daemon (the schema can't json-parse).

See [L13 plan §2.D1](../../../../log/L13/plan.md) for the full
key list; the schema file itself is the canonical reference.

## Module layout

```
modules/net/router/
├── CMakeLists.txt
├── inc/
│   └── router.hpp              v0 public API (run_daemon, dump)
├── src/
│   ├── main.cpp                CLI parse + entry
│   ├── main_impl.cpp           v0 dump-net-keys (D2)
│   ├── ds_bridge.{hpp,cpp}     net.* DsBridge — D3, pending
│   ├── nft_rules.{hpp,cpp}     pure ruleset generator — D4 (partial)
│   ├── shell.{hpp,cpp}         popen()-backed Runner abstraction — D5
│   ├── ip_route.{hpp,cpp}      `ip route replace` metric writer — D5
│   ├── iface_monitor.{hpp,cpp} `ip -j link/route show` parser — D5
│   ├── apply.{hpp,cpp}         tempfile + `nft -f` apply wrapper — D6
│   ├── lifecycle.{hpp,cpp}     pure FSM: Sinks + Inputs → step() — D6
│   └── daemon.cpp              run_daemon() — poll loop, signals — D7
├── schemas/net.lua
├── test/
│   ├── ds_bridge_test.cpp      D3
│   ├── nft_rules_test.cpp      D4
│   ├── iface_monitor_test.cpp  D5
│   ├── ip_route_test.cpp       D5
│   ├── apply_test.cpp          D6 (uses a real fake-nft shell script)
│   └── lifecycle_test.cpp      D6 (FSM transitions + recovery)
└── docs/design.md              this file
```

## Related docs

- [L13 plan](../../../../log/L13/plan.md)
- [data-store protocol](../../../data-store/docs/protocol.md)
- [data-store client API](../../../data-store/docs/client_api.md)
- [openvpn-client design](../../../openvpn/client/docs/design.md) — same DsBridge pattern + kernel-ownership story
