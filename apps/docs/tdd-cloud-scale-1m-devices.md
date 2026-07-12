# TDD — Scaling the cloud to 1M devices

Status: **PROPOSAL — not started** · Target: cloud (`iot-cloudd`, `iot-httpd`,
`iot-ds`, OpenVPN) · Author: 2026-07-12

Companion to `tdd-device-ui-path-proxy.md` (which already replaces the per-device
port with a path-scoped proxy) and `tdd-multi-tenant-cloud.md` (per-tenant
subnets). This doc is about the ceilings those two leave standing.

---

## 1. Goal

Support **1,000,000 provisioned devices** on one cloud-iot deployment, with the
operator console, device-UI access, LwM2M DM and telemetry all working.

The honest headline: **today the cloud caps at 51 devices**, and three of the
five ceilings below cannot be raised by tuning a value — they are structural.
Nothing here is a config change plus a bigger VM.

### The scale we are actually designing for

| Quantity | At 1M devices |
|---|---|
| Provisioned devices | 1,000,000 |
| Devices **an operator is looking at**, concurrently | ~10²  (hundreds) |
| LwM2M re-registrations (24h lifetime, `bootstrap.cpp:28`) | 1e6 / 86400 ≈ **12/sec** sustained |
| `cloud.endpoints` JSON, at ~300 B/entry | **~300 MB**, in one ds key |
| nftables DNAT rules, one per device | **1,000,000 rules**, flushed + rebuilt per change |

The second row is the load-bearing one. **Device count and concurrent-access
count differ by four orders of magnitude**, and every ceiling below exists
because the current design conflates them — it provisions a permanent,
individually-addressable inbound path for every device, whether anyone is
looking at it or not.

---

## 2. The five ceilings

### C1 — Proxy port pool: caps at 51 today, ~55k *ever* (STRUCTURAL)

`cloud.vpn.proxy.port.start` = 10000, `.end` = 10050 (`cloud.lua:508-518`) →
**51 ports**. Each device is allocated one at provision time, and the pool
running dry does **not** merely cost the device its "Launch UI" button —
provisioning fails outright:

```cpp
// modules/server/openvpn/src/vpn_registry.cpp:73
if (m_free_ips.empty() || m_free_ports.empty()) return std::nullopt;

// modules/server/lwm2m/src/bootstrap.cpp:36-37
auto alloc = ...;
if (!alloc.has_value()) return std::nullopt;   // subnet / ports exhausted
```

Device #52 gets no tunnel IP, no BS PSK, no bootstrap. It cannot onboard at all.

Widening the range is bounded twice over. Practically: every published port
spawns a `docker-proxy` process on the host (`docker-compose.yml:74` publishes
the whole range), which is why the schema comment warns *"a wide range exhausts
the host."* And absolutely: **a TCP port number is 16 bits.** One port per device
cannot exceed ~55k devices at any budget, on any hardware, forever. 1M is 18×
past the physical ceiling.

**This model cannot be scaled. It must be deleted.** The good news is that its
replacement already exists and ships: the path-scoped proxy (`/dev/<ep>/...` →
`handler_proxy.cpp` → `dev_tun_ip:8080`) is *endpoint-addressed* and consumes
**zero** ports. `tdd-device-ui-path-proxy.md` already declares the DNAT path a
one-release transition fallback. Finishing that removal is P0 here.

### C2 — VPN tunnel IP pool: caps at 254 (TUNABLE, then STRUCTURAL)

`cloud.vpn.subnet` = `10.9.0.0/24` (`cloud.lua:365-368`) → **254 usable IPs**.
Multi-tenant makes this worse, not better: it carves a **/24 per tenant** out of
`cloud.vpn.tenant.pool` (`10.9.16.0/20`) → 16 tenants × 254 devices.

Widening to a /12 (`10.0.0.0/12` = 1,048,576 addresses) is arithmetically
sufficient and is a one-line default change. But it only moves the problem to C3,
because the reason every device needs a stable IP at all is that the cloud
expects to dial *inbound*.

### C3 — One `openvpn` process, one `tun0`, 1M permanent tunnels (STRUCTURAL)

Every provisioned device holds a **permanent TCP tunnel** to a single OpenVPN
process whose `tun0` lives in the `iot-cloudd` netns (shared into `iot-httpd` via
`network_mode: "service:iot-cloudd"`). 1M concurrent tunnels on one process and
one tun device is not a tuning problem; it is several orders of magnitude outside
what that design point supports. TCP-mode tunnels also carry the TCP-over-TCP
meltdown risk already documented in `project_vpn_udp_transport_parked`.

**C3 is the root cause of C1 and C2.** Kill the always-on inbound tunnel and both
the port pool and the IP pool stop being needed at all.

### C4 — `cloud.endpoints` is a single JSON blob, re-serialized per change (STRUCTURAL, and the most urgent)

This is the sharpest edge in the current code and it bites long before 1M.

`cloud.endpoints` is **one ds key holding a JSON array of every device**
(`cloud.lua:25-28`). Three separate things do O(N) work on it:

1. **Every endpoint change rewrites the entire array.** `sync_endpoints_to_ds`
   (`apps/cloud/server/src/main.cpp:109-130`) walks `reg.list_all()`, builds the
   whole array, and `ds.set()`s the lot. A single device flipping online →
   serialize ~300 MB → one ds `set` → **pushed to every watcher**, including
   every connected operator UI.
2. **Every proxied request re-parses the whole blob** to find one field —
   `resolve_dev_tun_ip` (`handler_proxy.cpp:85-96`) does
   `json::parse(ds_str(ds, "cloud.endpoints", "[]"))` and linear-scans for a
   matching `endpoint`. That is a ~300 MB parse **per HTTP request** to a device
   UI.
3. **The DNAT ruleset is rebuilt from it** (`apps/cloud/server/src/main.cpp:184-186`)
   → flush + rebuild 1M nft rules on every endpoint change.

C4 must be fixed **regardless of which long-term architecture wins**, it is the
cheapest of the structural fixes, and it is independently useful at 10k devices
let alone 1M. It is P1.

### C5 — Cloud front end and control plane (TUNABLE → needs sharding)

- `iot-httpd` is a hand-rolled ACE HTTP/1.1 server with `http.workers` default
  **4**, max **64** (`http.lua:55-56`), and each blocking long-poll pins a worker
  for its duration (`handler.cpp:987-1017`). Keep-alive is opt-in on an explicit
  header (`session.cpp:104-105`). This is sized for one operator console, not a
  fleet front end.
- `iot-ds` is a single process holding the whole keyspace.
- LwM2M DM terminates 1M DTLS peers on tinydtls, whose peer table is a fixed
  array (`DTLS_PEER_MAX`) — see `project_dtls_cannot_add_peer_wedge` for what
  happens when it fills. Exact cap **[VERIFY]**.

Note the one number that is *not* alarming: 12 re-registrations/sec sustained is
entirely tractable. The control plane's problem is **concurrency and fan-out**,
not request rate.

---

## 3. What already scales (do not touch)

- **The path-scoped proxy's addressing model.** `/dev/<ep>/` is endpoint-keyed
  and portless. It is the right shape; only its `cloud.endpoints` lookup (C4) is
  wrong.
- **Its authorization posture.** `handler_proxy.cpp:179-185` gates on the cloud
  operator session and 302s otherwise. See §6 — the DNAT path does *not* have
  this property.
- **The data-store watch/push mechanism.** `Client::watch(keys, EventCallback)`
  already delivers real pushes on a listener thread. The fan-out primitive is
  fine; the *payload* (C4) is what's broken.
- **BS HKDF zero-touch** (`tdd-bs-hkdf-zerotouch.md`). Stateless per-serial PSK
  derivation means onboarding does **not** require a pre-created row per device.
  This is exactly the right property at 1M and should be the default at scale.

---

## 4. Target architecture — on-demand reverse connect

Stop giving every device a permanent inbound path. Devices hold **only the
LwM2M/DTLS control channel**; an inbound path is created **on demand, for the
duration of an operator session, and torn down on idle**.

```
  Today:   1M devices × permanent tunnel + tunnel IP + TCP port   →  dies at 51
  Target:  1M devices × control channel only
           +  ~10² concurrent relay sessions, created on click     →  scales with OPERATORS
```

Flow:

1. Operator clicks **Launch UI** for endpoint `<ep>` in the console.
2. Cloud issues an **LwM2M Execute** on `<ep>`'s control channel: *"dial a relay
   session, token `<T>`"*.
3. Device dials **outbound** to a relay tier and presents `<T>`.
4. The relay binds that session to the operator's session; `handler_proxy.cpp`
   routes `/dev/<ep>/...` to the bound relay session instead of to a tunnel IP.
5. Idle timeout → session torn down; the device holds nothing.

What this deletes outright: the tunnel-IP pool (C2), the proxy-port pool (C1),
the nftables DNAT table and its rebuild-per-change (part of C4), and 1M permanent
OpenVPN tunnels (C3). Concurrency then scales with the number of operators, which
is the quantity that is actually ~10².

**This is an architecture change, not a patch.** It is sequenced last on purpose:
P0–P2 below are each independently valuable, and none of them are wasted if the
reverse-connect design is later revised.

---

## 5. Phased plan

### P0 — Stop the bleeding (small, ship first)

| # | Change | Files |
|---|---|---|
| P0a | Bind the published proxy range to **loopback** (see §6) | `apps/cloud/docker-compose.yml:74` |
| P0b | Make `proxy_port` allocation **non-fatal**: allocate when the pool has a free port, else `proxy_port = 0`, skip that device's DNAT rule, and let provisioning **succeed** | `vpn_registry.cpp:73,97`; `bootstrap.cpp:36` |
| P0c | Widen `cloud.vpn.subnet` /24 → /16, and the per-tenant carve /24 → /20 | `cloud.lua:365-368`, `cloud.vpn.tenant.pool` |

P0b alone removes the 51-device onboarding cliff **today**: devices past the pool
lose only their DNAT fallback, and the path proxy — the gated path — carries them
anyway. P0 gets the deployment to ~65k devices honestly.

### P1 — Kill the `cloud.endpoints` blob (C4)

| # | Change |
|---|---|
| P1a | Split into **per-endpoint keys** — `cloud.endpoint.<ep>` — so a change writes O(1), not O(N), and watchers receive only the row that moved |
| P1b | Replace `resolve_dev_tun_ip`'s full-blob parse (`handler_proxy.cpp:85-96`) with a single-key `ds.get("cloud.endpoint." + ep)` |
| P1c | Give the console a **paged** endpoint list (`GET /api/v1/cloud/endpoints?offset=&limit=`) backed by Mongo, not by reading one giant ds key |
| P1d | Drop the DNAT rebuild-from-`cloud.endpoints` loop (`main.cpp:184-186`) — falls out of P2 |

P1 is the highest value-per-line work in this document and is **not** contingent
on the reverse-connect decision.

### P2 — Retire the per-device port + DNAT entirely (C1)

Finish what `tdd-device-ui-path-proxy.md` §"phase-2 follow-up" already scheduled:
delete `modules/server/dnat/`, the `cloud.vpn.proxy.port.*` keys, the
`cloud.vpn.port.next` counter, `proxy_port` from `EndpointInfo`, and the compose
published range. The path proxy becomes the only route to a device UI.

### P3 — Reverse connect (C3, C2)

Design and build §4. Devices hold the control channel only; relay sessions are
created on demand. Prerequisite: P2 (nothing else may depend on a stable inbound
tunnel IP).

### P4 — Horizontal control plane (C5)

Shard `iot-httpd` / `iot-ds` / the DM behind a real front end; the endpoint→shard
mapping is a natural extension of P1's per-endpoint keys. Out of scope for the
first pass — **[VERIFY]** what tinydtls's `DTLS_PEER_MAX` actually is before
sizing a DM shard.

---

## 6. Security note — the published range is internet-facing

Independently of scale, this is worth fixing now:

```yaml
# apps/cloud/docker-compose.yml:74
- "${PROXY_START:-10000}-${PROXY_END:-10050}:${PROXY_START:-10000}-${PROXY_END:-10050}"
```

No `127.0.0.1:` bind → the range is published on **all interfaces**. Every
provisioned device's UI login page is therefore directly reachable from the
internet at `<cloud-public-ip>:1000N`, and that route **bypasses the cloud
operator session gate** that `handler_proxy.cpp:179-185` enforces on the
`/dev/<ep>/` path. The device's own auth is the only barrier left.

P0a (loopback bind) closes it; P2 (delete the range) closes it permanently.

---

## 7. Test plan

- **Unit** — `vpn_registry`: port-pool exhaustion returns an allocation with
  `proxy_port == 0` rather than `nullopt` (P0b); IP-pool exhaustion still fails.
  `bootstrap`: provision succeeds with a dry port pool and yields a usable
  tunnel IP + PSK.
- **Unit** — per-endpoint ds keys: write/read/delete `cloud.endpoint.<ep>`;
  `resolve_dev_tun_ip` returns the right IP with 100k rows present and does not
  read `cloud.endpoints`.
- **Load** — extend `tdd-cloud-load-benchmark.md`: provision 100k synthetic
  endpoints; assert (a) provision latency is flat vs. endpoint count, (b) a
  `/dev/<ep>/` request's server-side resolve time is flat, (c) one endpoint state
  flip does not push an O(N) payload to watchers. These three assertions are
  exactly the C4 regression guards.
- **E2E** — device #52 onboards (BS → DM → registered) on a default port pool,
  and its UI is reachable via `/dev/<ep>/` with no DNAT rule installed.

---

## 8. Open items

1. Relay tier for P3: reuse OpenVPN with on-demand client sessions, or a purpose-
   built token-authenticated relay? The latter is likely smaller and avoids
   TCP-over-TCP, but it is new code on the operator-access path.
2. Does the LwM2M Execute → device-dials-out handshake need a new object, or can
   it ride an existing one? **[VERIFY]** against `lwm2m-object-handling.md`.
3. `DTLS_PEER_MAX` in the vendored tinydtls — actual value, and whether the DM
   can be sharded by endpoint hash without breaking the DTLS session cache.
4. At 1M devices, is Mongo the endpoint index, or does the ds keyspace stay
   authoritative with Mongo as the mirror (today's split)?
5. Per-tenant scale: is 1M *per tenant*, or 1M across all tenants? The subnet
   carve in `tdd-multi-tenant-cloud.md` assumes the latter.

---

## 9. What this document does not cover

No telemetry/ingest scaling (the vehicle-telemetry NDJSON spool and the 60-day
Mongo record have their own ceilings — see `tdd-vehicle-telemetry.md`), no OTA
fan-out at 1M devices, no cost model, no multi-region. Those are separate docs.
