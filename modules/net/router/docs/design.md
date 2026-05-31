# net-router — module design (L13)

> **Status (2026-05-31):** D1 + D2 scaffold landed. D3 (DsBridge),
> D4 (nft_rules), D5 (ip_route + iface_monitor), D6 (lifecycle +
> e2e smoke), D7 (packaging) pending.

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
│   ├── nft_rules.{hpp,cpp}     pure ruleset generator — D4, pending
│   ├── ip_route.{hpp,cpp}      metric write wrapper — D5, pending
│   ├── iface_monitor.{hpp,cpp} `ip link show -j` parser — D5, pending
│   └── apply.{hpp,cpp}         `nft -f -` invoker — D5, pending
├── schemas/net.lua
├── test/
│   └── placeholder_test.cpp    keeps net-router-tests linkable
│                               until D3 lands ds_bridge_test.cpp
└── docs/design.md              this file
```

## Related docs

- [L13 plan](../../../../log/L13/plan.md)
- [data-store protocol](../../../data-store/docs/protocol.md)
- [data-store client API](../../../data-store/docs/client_api.md)
- [openvpn-client design](../../../openvpn/client/docs/design.md) — same DsBridge pattern + kernel-ownership story
