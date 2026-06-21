# TDD Plan — Vehicle Telemetry over CAN (ISO 15765-4 / OBD-II)

Status: **PLAN / proposed** (2026-06-21). Greenfield — no CAN code exists in
the repo today.

## 1. Goal

Read live vehicle telemetry from the OBD-II port over CAN (ISO 15765-4),
publish it to the data-store, push it to **cloud-iot over LwM2M** (for a live
map + dashboards), and **optionally mirror it to an MQTT broker**. The MQTT
client ships **disabled by default** and starts only once a broker is
configured + saved.

### v1 scope decisions (confirmed)

| Question | Decision |
| --- | --- |
| CAN interface | **SocketCAN** (`can0`) — MCP2515 SPI HAT or USB-CAN; OBD PIDs over raw CAN, DTCs over ISO-TP |
| Map | **Live position** (v1) + **history/replay** (v2) — device buffers, cloud is the 60-day store |
| Cloud cadence | **~1 Hz** batched poll → cloud |
| Signals (v1) | Core powertrain (speed/RPM/coolant/throttle/load), Fuel & intake (fuel level/IAT/MAF), **DTCs** (Mode 03), **GPS** (reuses existing cellular GPS → Object 6) |

Historical track/replay moves to **v2** (cloud-stored, §3b). Non-goals overall:
J1939 (heavy-duty), bidirectional control / actuation, OBD security (Mode 27).

## 2. Architecture (three planes)

```
  OBD-II / CAN bus
        │
        ▼
  iot-vehicled (NEW daemon, privileged: CAP_NET_RAW, owns can0)
   │  ISO 15765-4 OBD-II PID poller — ACE_Reactor on the SocketCAN fd,
   │  1 Hz timer issues PID requests round-robin (mirrors cellular_client)
   │
   ├─► data-store (VOLATILE)  vehicle.rpm / vehicle.speed / vehicle.coolant …
   │      │                   (+ persistent vehicle.dtc on change)
   │      │
   │      ├─► lwm2m client  reader-hook lambdas (LocationHooks pattern) →
   │      │      Vehicle LwM2M object + GPS Object 6 → cloud-iot → MAP
   │      │
   │      ├─► iot-mqttd (NEW, OFF by default)  watch vehicle.* → publish
   │      │      JSON to broker, topic <serial>/<user-suffix>
   │      │
   │      └─► TelemetryMirror (NEW ACE_Task)  non-blocking post() → on-device
   │             MongoDB = store-and-forward BUFFER (short TTL safety net)
   │                   │
   │                   └─► LwM2M 1.1 SEND (SenML batch) ─► cloud lwm2m-dm
   ▼                            │  ds inbox key  │
  can0 up (oneshot)             ▼                ▼
                         iot-httpd drains ─► CLOUD MongoDB (native time-series,
                                              60-day TTL) ◄─ system of record
                                              │
                                              └─► cloud-ui Map + history/replay
```

**Two-tier storage:** the **device MongoDB is a short store-and-forward buffer**
(offline-tolerant; prune points once the cloud durably ACKs them, plus a small
TTL safety net). The **cloud MongoDB is the 60-day system of record** — and
because the cloud is the **x86 Vultr VM**, it runs **mongod ≥ 5.0 → native
time-series collections** (the device's ARM/4.4 limitation, §3a, does not apply
to the cloud). The map now shows **live position AND history/replay** from the
cloud store.

Why two new daemons (not one): **privilege split** (the repo's rule —
"unprivileged client reads ds, privileged daemons own the hardware"). The CAN
daemon needs `CAP_NET_RAW`; the MQTT daemon needs only the network + ds. Keeping
them separate means a broker outage can't stall CAN ingest and vice-versa.

## 3. Data-store strategy (answers "can ds handle high-speed R/W?")

Hard facts from the ds implementation:

- **Persistent `set` = full-file rewrite + `fsync`** (`lua_persistor.cpp:167`),
  design target **≤10 writes/s** (`design.md:276`). NOT for high-rate.
- A **volatile (in-memory, no-fsync) path exists** — per-request `volatile:true`
  flag (`worker.cpp:257`, `data_store.hpp:60`): ~14k ops/s, 248k keys/s batched.
  Lost on restart — perfect for transient live telemetry.
- **No JSON-object Value type** (`value.hpp:24`) — a batched payload must be a
  JSON **string** value.

**v1 (1 Hz) policy — keep it idiomatic, not over-engineered:**

- **Live signals → individual VOLATILE keys** (`vehicle.rpm`, `vehicle.speed`,
  …), exactly like `iot.sensor.*` / `gps.*`. At ~8 keys/s this is trivial and
  needs no batching. Volatile = no SD-card wear for data that's meaningless
  after the next tick.
- **DTCs + config → PERSISTENT** (low rate, on-change only): `vehicle.dtc`,
  `vehicle.*` config keys.
- **Cloud side** reads at the LwM2M observe cadence (≈1 Hz) — also written
  **volatile** into the cloud ds (latest-wins; the map only needs latest).

**If a future phase needs high rate (>10 Hz):** switch the live signals to a
single batched JSON-string key written volatile once per tick (~2.4 µs/key
amortized), and downsample the LwM2M/cloud stream. Recorded here so the v1
per-key choice is a deliberate, reversible simplification — not a ceiling.

## 3a. Storage plane — on-device MongoDB telemetry history

**Decision: reuse iot's existing `DbClient`** (`apps/inc/db_adapter.hpp`,
mongocxx) — it is **already compiled into the device image**
(`IOT_ENABLE_MONGO=ON`, `mongo-c-driver`/`mongo-cxx-driver` Yocto recipes
wired). No new dependency.

**Insert path: clone the proven async pattern.** Add
`TelemetryMirror : public ACE_Task<ACE_MT_SYNCH>`, a near-copy of
`lwm2m::RegistryMirror` (`apps/src/lwm2m_registry_mirror.cpp`): a bounded queue
(`kHighWater`), non-blocking `post(record)` called from the vehicle daemon's
reactor tick, and a `svc()` worker that drains the queue and calls
`DbClient::create_documents()` (batch insert). On overflow → drop + count
(telemetry is lossy-tolerant; the live ds copy stays authoritative). This also
fills in `RegistryMirror`'s currently-stubbed `persist()` by example.

- **Role: store-and-forward BUFFER, not the long-term store.** Each point is
  marked `sent:false`; once the cloud durably ACKs it (§3b) the daemon flips it
  `sent:true` and prunes (or lets a short TTL reap it). An "upload watermark"
  (last-acked `ts`/seq) drives oldest-first batch uploads + survives restarts.
- **Collection:** `telemetry` (db `iot`), document per poll:
  `{ "ts": ISODate, "ep":"<serial>", "seq":N, "sent":false, "speed":…, …, "lat":…, "lon":… }`.
- **Retention:** short **TTL safety net** (e.g. 24–48 h) so an extended offline
  stretch can't lose data, plus prune-on-ACK. Time-series collection where the
  device mongod supports it, else a TTL-indexed normal collection (§3a caveat).
- **Connection:** new ds key `iot.mongo.uri` (e.g.
  `mongodb://127.0.0.1:27017/iot`), read at startup → `DbClient(uri)`. Empty →
  buffer disabled (telemetry still flows to ds/LwM2M/MQTT; just no local spool).
- Guard all new code with `#ifdef IOT_ENABLE_MONGO` so the cloud build (Mongo
  OFF) still compiles.

> ⚠️ **Hardware caveat — Mongo version vs RPi CPU.** Native **time-series
> collections need mongod ≥ 5.0**, and mongod ≥ 5.0 requires **ARMv8.2-A**.
> The **RPi 3B/4 (Cortex-A53/A72) is ARMv8.0-A**, so the newest mongod that
> runs there is **4.4** — which has **TTL indexes but NOT time-series
> collections**. So the on-device server likely caps at 4.4. **Design for both:**
> create a time-series collection where supported, else fall back to a normal
> collection with a TTL index (`expireAfterSeconds`) — identical insert code,
> only the one-time collection-creation differs. Confirm what mongod (and which
> arch) actually runs on the target before PR-7. (If Mongo runs in a container
> or off-device, 5.0+ may be available.)

## 3b. Cloud telemetry pipeline — LwM2M Send → cloud MongoDB (60-day)

**Transport: LwM2M 1.1 Send (client-initiated batch push).** The device drains
its buffer oldest-first and pushes batches as a **SenML pack** via LwM2M **Send**
(CoAP POST to `/dp`) — multiple timestamped records in one message, exactly what
Send is for. Cadence: opportunistic (when the tunnel is up); offline → the
buffer grows and flushes on reconnect (backfill).

> ⚠️ **Prerequisite — Send + SenML are NOT implemented yet.** They're on the
> deferred-interop list (`apps/docs/leshan-interop.md`: SenML/CBOR, LwM2M Send).
> This choice makes **implementing client-side Send + a SenML pack codec** (and
> the server-side `/dp` receive) the **critical path** for cloud history.
> **Fallback** if Send is too heavy for v1: a custom batch Object resource the
> device fills and `lwm2m-dm` pulls via Observe/Notify — uglier, no new verb.

**Cloud path (chosen: ingest in `iot-httpd`, Mongo as a compose service):**

```
device ──Send(SenML)──► lwm2m-dm ──ds inbox key──► iot-httpd ──► cloud MongoDB
            (CoAP /dp)      │  cloud.telemetry.inbox   (drain)     (time-series,
            ◄── 2.04 ACK ───┘  (volatile JSON batch)               60-day TTL)
```

- `lwm2m-dm` receives the Send, appends the batch to a **volatile** ds key
  `cloud.telemetry.inbox`, and **2.04-ACKs only after the handoff** (so the
  device won't prune un-handed-off data). Sole-writer discipline as with
  `cloud.lwm2m.registrations`.
- **`iot-httpd` owns the Mongo write** — watches `cloud.telemetry.inbox`, batch-
  inserts into the cloud `telemetry` time-series collection, **dedups on
  `(endpoint, ts)`** (idempotent — a re-sent batch never double-writes).
- **Cloud Mongo deployment:** add a **`mongod` service to
  `apps/cloud/docker-compose.yml`** (x86 VM → mongod ≥ 5.0 → native time-series
  + `expireAfterSeconds = 5184000` (60 d)). `iot-httpd` links **mongocxx**
  behind a new build flag (e.g. `IOT_HTTPD_MONGO=ON` cloud-only) so the device
  httpd stays lean. Reuse the same `DbClient` (lift it to a shared spot, or a
  thin copy in `modules/http-server`).
- **ACK-then-prune end to end:** device prunes a point only after the Send it
  rode in got a 2.04; the cloud's dedup makes an un-acked re-send safe.
- cloud-ui Map reads **live** position from `cloud.vehicle.telemetry` (volatile)
  and **history/replay** from Mongo via a new `iot-httpd` query endpoint.

## 4. New data-store keys

### Device (`iot.lua` or a new `vehicle.lua` schema)

| Key | Type | Persist | Notes |
| --- | --- | --- | --- |
| `vehicle.can.iface` | string | yes | SocketCAN iface, default `can0` |
| `vehicle.can.bitrate` | uint32 | yes | default `500000` (ISO 15765-4); 250000 variant |
| `vehicle.poll.interval.ms` | uint32 | yes | default `1000` |
| `vehicle.pids` | string | yes | CSV of enabled PIDs, default core+fuel set |
| `vehicle.rpm` | string | **volatile** | engine RPM |
| `vehicle.speed` | string | **volatile** | km/h |
| `vehicle.coolant` | string | **volatile** | °C |
| `vehicle.throttle` | string | **volatile** | % |
| `vehicle.load` | string | **volatile** | engine load % |
| `vehicle.fuel` | string | **volatile** | fuel level % |
| `vehicle.iat` | string | **volatile** | intake air temp °C |
| `vehicle.maf` | string | **volatile** | g/s |
| `vehicle.dtc` | string | yes | JSON array of active DTCs (Mode 03), on-change |
| `vehicle.link` | string | **volatile** | `up`/`down`/`no-ecu` (bus health for UI) |
| `iot.mongo.uri` | string | yes | on-device Mongo URI; empty → history disabled |
| `vehicle.history.enable` | bool | yes | default true; gates the TelemetryMirror insert |
| `vehicle.history.ttl.sec` | uint32 | yes | retention, default `604800` (7 days) |

(Values are formatted decimal strings, matching the `iot.sensor.*` convention,
so the existing ds-hint/debug + LwM2M string-reader plumbing applies unchanged.)

### MQTT (`mqtt.lua` schema)

| Key | Type | Notes |
| --- | --- | --- |
| `mqtt.broker.host` | string | empty default → daemon parks |
| `mqtt.broker.port` | uint32 | default `1883` (`8883` for TLS) |
| `mqtt.broker.user` | string | optional |
| `mqtt.broker.pass` | string | optional, `write_acl` gid-gated, write-only |
| `mqtt.tls.enable` | bool | default false |
| `mqtt.client.id` | string | default = serial |
| `mqtt.mirror.enable` | bool | default false — the "mirror telemetry" toggle |
| `mqtt.topic.suffix` | string | user-configured; full topic = `<serial>/<suffix>` |
| `services.mqtt.enable` / `services.mqtt.state` | — | ServiceGate (default **false**) |
| `services.vehicle.enable` / `services.vehicle.state` | — | ServiceGate (default false; UI enables on CAN config save) |

## 5. Component breakdown (proposed PRs)

### Phase 1 — `iot-vehicled` daemon + device-ui read-only [DEVICE]

New module `modules/vehicle/` (mirrors `modules/wan/cellular/`):
- `daemon/vehicle_client.{hpp,cpp}` — `ACE_Event_Handler` subclass; opens
  `socket(PF_CAN, SOCK_RAW, CAN_RAW)` bound to `can0`, registers the fd with
  `ACE_Reactor::instance()`; `handle_timeout()` (1 Hz) sends the next PID
  request (functional ID `0x7DF`), `handle_input()` decodes responses (`0x7E8`)
  and publishes volatile ds keys. `ServiceGate("vehicle")` parks when disabled
  or `can0` is absent.
- `obd/pid_decode.{hpp,cpp}` — pure PID encode/decode (unit-testable, no I/O).
  See §6 table.
- `obd/isotp.{hpp,cpp}` — ISO-TP socket (`CAN_ISOTP`) for **Mode 03 DTCs**
  (multi-frame). PIDs are single-frame on `CAN_RAW`; DTCs need ISO-TP.
- Schema `vehicle.lua`; systemd unit `iot-vehicled.service`
  (`AmbientCapabilities=CAP_NET_RAW`, `SupplementaryGroups=iot`, **no**
  `RuntimeDirectory=iot` — tmpfiles owns `/run/iot`); a `iot-can0-up.service`
  oneshot to `ip link set can0 up type can bitrate <n>`.
- device-ui **Vehicle** page (read-only live values), on the `vpn-config`
  observe pattern.
- Tests: `pid_decode` gtest (golden OBD frames → expected values).

### Phase 2 — LwM2M bridge + cloud map [DEVICE + CLOUD]

**Decision: one custom single-instance Vehicle Telemetry object** — not a
spray of IPSO instances. OBD has no standard IPSO object, and mixing
3303/3320/… instances for coolant/throttle/etc. is messy to observe and map.
Object ID **`33000`** (LwM2M **private-use** range 32769–42768 — unregistered,
safe; distinct from the repo's existing OMA-3rd-party Object 2048 VPN push).

`/33000/0` resources (all `R` + observable; floats are SenML-friendly):

| RID | Resource | Unit | ds source |
| --- | --- | --- | --- |
| 0 | Speed | km/h | `vehicle.speed` |
| 1 | RPM | rpm | `vehicle.rpm` |
| 2 | Coolant temp | °C | `vehicle.coolant` |
| 3 | Throttle | % | `vehicle.throttle` |
| 4 | Engine load | % | `vehicle.load` |
| 5 | Fuel level | % | `vehicle.fuel` |
| 6 | Intake air temp | °C | `vehicle.iat` |
| 7 | MAF | g/s | `vehicle.maf` |
| 8 | DTC list | — | `vehicle.dtc` (JSON string) |
| 9 | MIL on | bool | `vehicle.mil` |
| 10 | Link state | — | `vehicle.link` |

- Bind reader-hook lambdas reading the `vehicle.*` ds keys — the exact
  `LocationHooks` pattern at `main.cpp:1006`. Register the object in
  `apps/docs/lwm2m-object-handling.md`.
- **GPS = Object 6** (already flows from the cellular daemon) — that *is* the
  map position; the Vehicle object is the telemetry overlay. (Note: OBD speed
  RID 0 ≠ GPS speed /6/0/6 — keep both, they differ.)
- cloud `lwm2m-dm` **observes** `/33000/0` + `/6/0` at ~1 Hz and merges latest
  values into a new **volatile** cloud key
  **`cloud.vehicle.telemetry`** = JSON array `[{endpoint, lat, lon, speed, rpm,
  …, ts}]`, latest-wins. Same sole-writer/merge discipline as
  `cloud.lwm2m.registrations` (lwm2m-dm writes; iot-cloudd needn't touch it).
- **cloud-ui Map page** — Leaflet markers from `cloud.vehicle.telemetry`
  (long-poll), telemetry popover per endpoint. New nav entry. Accepts a focus
  param `?ep=<serial>` → centers + opens that endpoint's marker.
- **Endpoints ↔ Map integration (kept lean — NO new columns/action buttons):**
  - Merge geographic **`lat`/`lon` (+ `pos_ts`)** into `cloud.endpoints` from
    **GPS Object 6**, via the existing sole-writer/merge discipline used for
    `lan_ip` (lwm2m-dm → `cloud.lwm2m.registrations`; iot-cloudd merges). This is
    **hidden gating data** — used only to decide whether the row is linkable,
    NOT shown as a column (avoids datagrid bloat).
  - **Make the endpoint NAME a hyperlink** when position is available → click
    plots it on the Map (`?ep=<serial>` → center + popover). Plain text when no
    position (non-GPS endpoints degrade cleanly). Zero extra columns/buttons —
    just changes how the existing endpoint cell renders.
  - **Leave the existing "Location" column as-is** — it is the LwM2M
    **registration / heartbeat URI** (`/rd/<id>`, the path the client posts
    registration Updates/heartbeats to; `endpoint_registry.hpp:41`), NOT a
    place. Optionally relabel its header to **"Reg URI"** for clarity; do **not**
    overload it with geo.
- DTCs surfaced in cloud-ui (persistent `vehicle.dtc`, RID 8).

### Phase 3 — `iot-mqttd` mirror [DEVICE + PKG]

- New module `modules/mqtt/` — **thin ACE adapter over libmosquitto** (do NOT
  lift grace-server's `mqtt_io`; it's `libevent`/`evt_io`-bound). ~100 lines:
  wrap the mosquitto socket fd in an `ACE_Event_Handler` (`handle_input` →
  `mosquitto_loop_read`, `handle_output` → `mosquitto_loop_write`) + a 1 Hz ACE
  timer → `mosquitto_loop_misc` (keepalive/reconnect). Reference:
  `grace-server/hackthon/app/src/mqtt_io.cpp`.
- `ServiceGate("mqtt")` default **disabled**. device-ui **MQTT** config page
  (broker host/port/creds/TLS, **mirror toggle**, topic suffix); on save the UI
  writes `mqtt.*` then sets `services.mqtt.enable=true` → daemon starts.
- When `mqtt.mirror.enable`, the daemon ds-watches `vehicle.*`, builds a JSON
  payload, and `publish()`es to `<serial>/<topic.suffix>` (QoS 0, configurable).
- **Yocto:** add a `libmosquitto` dependency (meta-openembedded ships a
  `mosquitto` recipe) + the new units to the image; `iot-sysusers` /
  privilege as needed (network + ds only).

**Decisions (MQTT):**

- **Two-level gate, by design:** `services.mqtt.enable` = *daemon runs +
  maintains the broker connection* (UI sets it true on Save **iff** broker host
  non-empty; false when host cleared). `mqtt.mirror.enable` = *publish vehicle
  telemetry or not*. This lets the connection exist for future use (remote
  commands) without forcing mirroring on. The `ServiceGate("mqtt")` parks the
  daemon whenever `services.mqtt.enable` is false or the host is empty.
- **Topic:** `<serial>/<mqtt.topic.suffix>` (default suffix `telemetry`) →
  e.g. `100000abcd/telemetry`. Plus a retained **status** topic
  `<serial>/status` driven by an MQTT **Last-Will** (`offline`) + `online` on
  connect, so a dashboard sees device liveness for free.
- **Payload:** ONE JSON object per poll (not per-signal topics):
  `{"ts":1718…, "speed":62, "rpm":2150, "coolant":89, …, "lat":…, "lon":…}`.
  Published **`retain=true`** (a late subscriber gets the last frame), **QoS 0**
  default (telemetry is lossy-tolerant; QoS configurable via `mqtt.qos`).
- **TLS:** schema keys land now (`mqtt.tls.enable`, `mqtt.tls.ca`), but v1
  implements **plain 1883**; 8883/CA is a fast-follow toggle.

## 6. OBD-II PID reference (Mode 01, single-frame)

Request (11-bit, ID `0x7DF`): `[0x02, 0x01, PID, 0,0,0,0,0]`.
Response (ID `0x7E8`): `[N, 0x41, PID, A, B, …]`.

| Signal | PID | Decode | ds key |
| --- | --- | --- | --- |
| Engine load | `0x04` | `A*100/255` % | `vehicle.load` |
| Coolant temp | `0x05` | `A-40` °C | `vehicle.coolant` |
| RPM | `0x0C` | `(256A+B)/4` | `vehicle.rpm` |
| Speed | `0x0D` | `A` km/h | `vehicle.speed` |
| Intake air temp | `0x0F` | `A-40` °C | `vehicle.iat` |
| MAF | `0x10` | `(256A+B)/100` g/s | `vehicle.maf` |
| Throttle | `0x11` | `A*100/255` % | `vehicle.throttle` |
| Fuel level | `0x2F` | `A*100/255` % | `vehicle.fuel` |

DTCs: **Mode 03** (request `0x03`), multi-frame over **ISO-TP**; decode the
2-byte DTC words to the standard `Pxxxx/Cxxxx/Bxxxx/Uxxxx` strings → JSON array
in `vehicle.dtc`. MIL/readiness via Mode 01 PID `0x01`.

## 7. Privilege, bring-up & packaging notes

- **CAN is a netdev, not a `/dev` node** — the daemon needs `CAP_NET_RAW` to
  open `PF_CAN` (no `DeviceAllow`). Run as `DynamicUser` + `AmbientCapabilities=
  CAP_NET_RAW` + `SupplementaryGroups=iot` (ds socket), or a static account.
- **`can0` bring-up** is a system task (`ip link set can0 up type can bitrate`),
  not the daemon's — a small `iot-can0-up.service` oneshot (Before=
  `iot-vehicled`), bitrate from `vehicle.can.bitrate`. Kernel needs the CAN
  controller driver (e.g. `mcp251x` for the SPI HAT) + `can`, `can-raw`,
  `can-isotp` modules in the Yocto image.
- **No `RuntimeDirectory=iot`** in any unit (known regression — tmpfiles owns
  `/run/iot 2775 root:iot`).
- DTCs/Mode 03 require the `can-isotp` kernel module (mainline since 5.10).

## 8. Suggested merge order (small PRs)

**v1 — live telemetry + map:**
1. `modules/vehicle/obd/pid_decode` + gtest (pure, no hardware) — lands the
   decode logic testable in CI first.
2. `iot-vehicled` daemon + `vehicle.lua` + units + ServiceGate (ingest →
   volatile ds). device-ui Vehicle page.
3. Vehicle LwM2M object `33000` + reader hooks (device) → cloud observe → cloud ds.
4. cloud-ui Map page (reuses GPS Object 6 + vehicle volatile keys).

**v1.1 — diagnostics, mirror, local buffer:**
5. DTC path (ISO-TP Mode 03 → `vehicle.dtc` → UI).
6. `iot-mqttd` + MQTT config page + Yocto `libmosquitto`.
7. **`TelemetryMirror` ACE_Task → on-device MongoDB buffer** (reuse `DbClient`;
   clone `RegistryMirror`); store-and-forward schema (`sent`/`seq`/watermark);
   TTL safety net; `iot.mongo.uri` config.

**v2 — cloud history pipeline (depends on Send):**
8. **LwM2M Send + SenML pack codec** (client push + server `/dp` receive) — the
   critical-path enabler; lands with its own gtest. (Or the Observe/Notify
   fallback per §3b.)
9. Device uploader: drain buffer oldest-first → Send batches → prune on 2.04-ACK.
10. Cloud: `mongod` in `apps/cloud/docker-compose.yml`; `iot-httpd` Mongo link
    (`IOT_HTTPD_MONGO`); `lwm2m-dm` → `cloud.telemetry.inbox` → iot-httpd drain →
    cloud time-series (60-day TTL) with `(endpoint,ts)` dedup.
11. cloud-ui Map history/replay (query endpoint over the cloud Mongo).

## 9. Open items to confirm before coding

Resolved this round: SocketCAN/can0; ~1 Hz; signal set
(powertrain+fuel/intake+DTC+GPS); **Vehicle object `33000`**; **one retained
JSON payload** per poll on `<serial>/<suffix>`; **reuse iot `DbClient`** +
`TelemetryMirror`. **Two-tier storage**: device Mongo = store-and-forward
buffer (short TTL); **cloud Mongo = 60-day system of record** (x86 → native
time-series). **Transport = LwM2M Send (SenML)**; **cloud ingest = iot-httpd +
mongod compose service**. **Endpoints page (lean)**: the endpoint NAME becomes a
map hyperlink when position is available (hidden `lat`/`lon` gating merged into
`cloud.endpoints` from Object 6) — no new columns/buttons; the existing
"Location" column stays the LwM2M reg/heartbeat URI (`/rd/<id>`).

Still to confirm before the relevant PR:

- **CAN controller hardware** (RPi MCP2515 HAT vs USB-CAN) — sets the kernel
  module + `iot-can0-up` details. (PR-2)
- **Bus speed** 500 vs 250 kbps — `vehicle.can.bitrate` default 500000, confirm
  per target vehicle. (PR-2)
- **On-device mongod version + arch** — time-series needs ≥5.0 (ARMv8.2-A); RPi
  3B/4 caps at 4.4 (TTL index only). Confirm what runs on the target, else use
  the TTL-index fallback. (PR-7) See §3a caveat.
- **TLS to broker** — plain 1883 in v1.1; 8883/CA fast-follow. (PR-6)
- **LwM2M Send + SenML** — confirm scope/effort vs the Observe/Notify fallback;
  this gates the whole cloud-history pipeline. (PR-8) See §3b.
- **Cloud mongod footprint** on the Vultr VM (RAM/disk for 60-day time-series at
  fleet scale) + `iot-httpd` mongocxx link via `IOT_HTTPD_MONGO`. (PR-10)
