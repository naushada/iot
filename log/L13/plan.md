# L13 Plan — net-router (nftables + interface priority)

> Forward-looking phase plan. Same shape as L11/L12. Greenfield
> `modules/net/router/` module. Watches `vpn.assigned.*` + `iot.*`
> via DsBridge, installs **nftables** rules forwarding to the
> lwm2m client, enforces interface priority for outgoing traffic
> via `ip route` metrics, and exposes a per-rule custom-rules
> escape hatch via a single JSON-encoded ds-server key.
>
> **Status (2026-05-31):** D1 + D2 done (PR #41). D3–D7 pending.

---

## 0. Goal

After L12 closed, an operator can:

```sh
ds-cli set vpn.remote.host '"vpn.example.com"' && systemctl enable --now iot-openvpn-client
```

…and a tunnel comes up with the assigned IP published as
`vpn.assigned.ip`. L13 extends that story: a new daemon watches the
data store, generates **nftables** rules so external traffic destined
for the device hits the lwm2m client on the tunnel side, and uses
`ip route` metrics to prefer **eth → wifi → cellular** for the device's
own outgoing requests. Custom forward/drop rules are an escape hatch
via a single JSON-encoded `net.custom_rules` key.

### Non-goals (first-cut scope)

- **iptables compatibility.** nftables is the modern path; L13 commits.
  An iptables-mode fallback is a FUP if a target ships without nftables.
- **Conntrack tuning, NAT64, IPv6 prefixes.** Standard `ip nat`
  forwarding only.
- **Bandwidth shaping / tc.** Out of scope.
- **Hot-reload of `net.custom_rules` mid-flight beyond regenerate-
  and-apply.** Full atomic transaction = stop accepting changes,
  apply, accept again. nftables makes this easy; we lean on it.
- **systemd-networkd / NetworkManager integration.** Out of scope
  — this daemon owns its own routing-table metric writes.

---

## 1. Risk register

| ID    | Risk                                                                                          | Mitigation                                                              |
|-------|------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------|
| R1    | nftables binary `nft` + kernel modules not present on minimal hosts                            | Containerfile + dev Dockerfile install `nftables`. Document kernel module reqs (`nft_nat`, `nft_chain_*`). |
| R2    | Daemon needs CAP_NET_ADMIN for both nftables AND `ip route` metric writes — same as openvpn-client | systemd unit ships `AmbientCapabilities=CAP_NET_ADMIN`; DynamicUser=yes is fine                          |
| R3    | Rule generation must be **idempotent** — re-applying a ruleset can't double-NAT or duplicate forwards | Generate a fresh `nft` ruleset under a dedicated table name; `nft flush table inet iot_router` then `nft -f -` the new ruleset in one transaction. |
| R4    | Interface up/down detection is racy if we just shell `ip link show`                            | Subscribe to netlink `RTM_NEWLINK` / `RTM_DELLINK` via libnl OR poll on 2s tick. First-cut: poll. Netlink subscription is FUP-L13-1. |
| R5    | `net.custom_rules` JSON parse errors must not crash the daemon — bad operator input is normal  | Catch nlohmann::json::parse_error, log via ACE_ERROR, keep old ruleset live. `net.state = "bad_custom_rules"` so operator can see via `ds-cli get`. |
| R6    | Interaction with other rule writers (NetworkManager, fail2ban, docker) — our flush will clobber their rules under the same table | Use a uniquely-named table (`inet iot_router`) so we never touch other writers' tables. nftables scopes by table name. |
| R7    | The lwm2m client's listen IP isn't `vpn.assigned.ip` — it's the lwm2m container's IP if running in a sibling container | First-cut: `net.lwm2m.target_ip` is operator-set explicitly. Auto-discovery is FUP-L13-2. |

---

## 2. D-items

### D1 — `net.*` schema

**Scope.** Add `modules/net/router/schemas/net.lua`. Auto-loaded by
ds-server from `/etc/iot/ds-schemas/net.lua`.

Read keys (operator → daemon):

| Key                       | Type    | Default              | Purpose                                                              |
|---------------------------|---------|----------------------|-----------------------------------------------------------------------|
| `net.tun.dev`             | string  | `"tun0"`             | TUN interface to monitor (matches openvpn's `dev`)                   |
| `net.lwm2m.target_ip`     | string  | (none, required)     | Where to DNAT inbound tunnel traffic                                 |
| `net.lwm2m.target_port`   | uint32  | 5684                 | Port on the target                                                   |
| `net.iface.priority`      | string  | `"eth,wifi,cellular"`| Comma-joined ordered list for outgoing traffic                       |
| `net.iface.eth.name`      | string  | `"eth0"`             | Actual kernel name to look up                                        |
| `net.iface.wifi.name`     | string  | `"wlan0"`            | Same                                                                  |
| `net.iface.cellular.name` | string  | `"wwan0"`            | Same                                                                  |
| `net.custom_rules`        | string  | `"[]"`               | JSON array of rule objects (see §3 below)                            |
| `net.poll.interval_sec`   | uint32  | 5                    | Interface-state poll cadence; netlink subscription is FUP-L13-1      |

Write keys (daemon → operator):

| Key                          | Type    | Meaning                                                                |
|------------------------------|---------|------------------------------------------------------------------------|
| `net.state`                  | string  | `monitoring` / `installing` / `bad_custom_rules` / `error` / `exited`  |
| `net.tun.ip`                 | string  | IP currently observed on `net.tun.dev`                                 |
| `net.tun.gateway`            | string  | Gateway currently observed                                             |
| `net.iface.active`           | string  | Highest-priority interface currently up (eth/wifi/cellular)            |
| `net.rules.applied_count`    | uint32  | Number of nft rules installed by this daemon (excluding system)        |
| `net.last_apply_unix`        | uint32  | Unix timestamp of last successful `nft -f -`                            |

### Custom rules JSON shape

```json
[
  {"action": "forward", "proto": "tcp", "dport": 5683, "to_ip": "10.8.0.6", "to_port": 5683},
  {"action": "drop",    "proto": "tcp", "dport": 23},
  {"action": "accept",  "proto": "udp", "sport": 53}
]
```

Fields: `action` (`forward`|`drop`|`accept`), `proto` (`tcp`|`udp`),
optional `dport`/`sport`/`to_ip`/`to_port`. Validation in code (the
schema only enforces `string`); diagnostics via ACE_ERROR on bad
shape + `net.state = "bad_custom_rules"`.

---

### D2 — Module scaffold

```
modules/net/router/
├── CMakeLists.txt
├── inc/router.hpp              v0 API (run_daemon, diagnostic dump)
├── src/
│   ├── main.cpp                CLI parse + entry
│   ├── main_impl.cpp           run_daemon glue
│   ├── ds_bridge.{hpp,cpp}     net.* DsBridge analog
│   ├── nft_rules.{hpp,cpp}     pure: build nft ruleset string from state
│   ├── ip_route.{hpp,cpp}      shell wrappers for `ip route replace`
│   ├── iface_monitor.{hpp,cpp} shell wrappers for `ip link show` parse
│   └── apply.{hpp,cpp}         `nft -f -` invoker; transactional apply
├── schemas/net.lua
├── test/                       gtest suite
└── docs/design.md
```

Binary: `net-router`. Standalone cmake module + integrated under
`apps/CMakeLists.txt::add_subdirectory` like data-store + openvpn.

---

### D3 — DsBridge (net.*)

Same pattern as `apps/src/ds_config.cpp` + `modules/openvpn/client/src/ds_bridge.cpp`.
Snapshot prime + callback watch on the read keys. Setters for the
write keys. Uses `data_store::to_string` / `data_store::to_uint32`
from PR #37.

**Tests.** Same shape as the other two DsBridge tests
(construct-against-bad-socket, missing_required for `net.lwm2m.target_ip`).

---

### D4 — `nft_rules` (pure rule-string generator)

Pure function:

```cpp
struct State {
    std::string tun_dev;            // "tun0"
    std::string tun_ip;             // observed
    std::string lwm2m_target_ip;
    std::uint32_t lwm2m_target_port;
    std::vector<CustomRule> custom;
};

std::string build_nft_ruleset(const State& s);
```

Output is a single nft script that gets piped to `nft -f -`:

```
flush table inet iot_router
table inet iot_router {
    chain prerouting { type nat hook prerouting priority -100; policy accept;
        iifname "tun0" tcp dport 5684 dnat to 10.8.0.6:5684
    }
    chain forward { type filter hook forward priority 0; policy accept;
        iifname "tun0" oifname "tun0" return
        ... custom rules ...
    }
}
```

**Tests.** Table-driven: known State → expected ruleset string.
Covers: empty custom rules, mixed forward/drop/accept, missing
required fields (target_ip empty → no DNAT chain).

---

### D5 — `ip_route` metrics + `iface_monitor`

`ip_route` shell-wraps `ip route replace default via <gw> dev <iface>
metric <N>` per the priority order: eth=100, wifi=200, cellular=300.
`iface_monitor` shell-wraps `ip -j link show <iface>` (JSON output)
to detect `OPER UP` per interface.

**Tests.** Stub out the shell command via dependency injection
(`std::function<std::string(const std::vector<std::string>&)>` runner
so tests pass canned `ip` output instead of executing).

---

### D6 — Main loop + e2e smoke

Periodic loop (every `net.poll.interval_sec`):
1. Poll iface monitor; if `net.iface.active` changed, re-apply route metrics.
2. Read current `net.tun.dev` from `ip link show` → update `net.tun.ip` if changed.
3. If any input to `build_nft_ruleset` changed, regenerate + `nft -f -` + bump `net.rules.applied_count` / `net.last_apply_unix`.
4. JSON-parse `net.custom_rules` on every iteration (cheap); on parse error → `net.state = "bad_custom_rules"` + keep old ruleset.
5. Exit cleanly on SIGINT/SIGTERM; on exit, `nft flush table inet iot_router` so we don't leak rules.

**E2E smoke** at `log/L13/net-router-smoke.sh`: boots ds-server +
fake `nft` shim (a shell that logs invocations to a file), seeds
required keys, runs `net-router --once`, asserts the fake nft was
called with the expected ruleset.

---

### D7 — Packaging integration

- `packaging/systemd/iot-net-router.service` — Type=simple, DynamicUser, AmbientCapabilities=CAP_NET_ADMIN, After=iot-ds.service Wants=iot-ds.service.
- `packaging/etc-iot/net-router.env` — `NET_ROUTER_ARGS="--ds-sock=/run/iot/data_store.sock"`.
- `packaging/iot-entrypoint.sh` — `IOT_ROLE=net` case.
- `packaging/Containerfile` — `apt install nftables` in the runtime stage; COPY the net-router binary.
- `docker/Dockerfile` — `apt install nftables` in the dev image.
- `apps/CMakeLists.txt` — `add_subdirectory(modules/net/router)` + install rules for the new unit + env file.
- `DEPLOY.md` — operator snippet for `iot-net-router.service`.

---

## 3. Open questions (track to closure)

| Q   | Question                                                                                       | Decision path                                                                       |
|-----|-------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|
| Q1  | nftables table family — `inet` or split `ip` + `ip6`?                                          | `inet` (covers both v4 + v6; one table, simpler).                                    |
| Q2  | DNAT for forwarded inbound — single port or port range?                                        | Single port for v1; range FUP if needed.                                             |
| Q3  | What if `net.iface.active` flaps frequently? Rate-limit the route-replace?                      | Don't act on changes within 1s of the last. Track `last_iface_change_unix`.          |
| Q4  | Should `nft -f` failures bring the daemon down or just mark `net.state = "error"` and retry?   | Latter — operator can fix custom_rules + we retry on next poll.                      |
| Q5  | netlink subscription instead of polling (FUP-L13-1) — worth it?                                | Only if `net.poll.interval_sec` ≤ 2 starts costing too much CPU on minimal hosts.    |

---

## 4. Sequencing

Suggested PR order, each independently mergeable:

1. **D1 + D2** — schema + scaffold (binary builds; `--dump` prints snapshot).
2. **D3** — DsBridge; binary refuses on missing required key.
3. **D4** — pure rule generator + table-driven tests.
4. **D5** — `ip route` + `iface_monitor`; tests via injected shell runner.
5. **D6** — main loop + fake-nft smoke harness; closes the FSM.
6. **D7** — packaging integration; ships as `IOT_ROLE=net`.

Estimated 6 PRs.

---

## 5. Related docs

- [L12 plan](../L12/plan.md) — openvpn-client; same DsBridge + sinks + lifecycle pattern reused here.
- [data-store protocol](../../modules/data-store/docs/protocol.md) — wire spec.
- [data-store client API](../../modules/data-store/docs/client_api.md) — `data_store::to_*` helpers in PR #37.
- [DEPLOY.md](../../DEPLOY.md) — top-level deploy walkthrough; D7 adds an `iot-net-router.service` section.
