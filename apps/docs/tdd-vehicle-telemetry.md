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
| Map | **Live position only** — latest lat/lon, no historical track / time-series store |
| Cloud cadence | **~1 Hz** batched poll → cloud |
| Signals (v1) | Core powertrain (speed/RPM/coolant/throttle/load), Fuel & intake (fuel level/IAT/MAF), **DTCs** (Mode 03), **GPS** (reuses existing cellular GPS → Object 6) |

Non-goals for v1: historical track replay, J1939 (heavy-duty), bidirectional
control / actuation, OBD security (Mode 27) — deferred.

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
   │      └─► iot-mqttd (NEW, OFF by default)  watch vehicle.* → publish
   │             JSON to broker, topic <serial>/<user-suffix>
   ▼
  can0 brought up by a oneshot unit: ip link set can0 up type can bitrate 500000
```

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

- Define a **Vehicle LwM2M object** (custom ID in the reusable range, e.g.
  `10350`) with one resource per signal; bind reader-hook lambdas reading the
  `vehicle.*` ds keys — the exact `LocationHooks` pattern at `main.cpp:1006`.
  (Evaluate reusing IPSO objects where a clean match exists; OBD has no standard
  IPSO object, so a custom object is expected.)
- GPS already flows to **Object 6** via the cellular daemon — the map reuses it.
- cloud `lwm2m-dm` observes the vehicle object + Object 6, writes **volatile**
  latest values into the cloud ds (latest-wins, per-endpoint).
- **cloud-ui Map page** — Leaflet (or Clarity map) plotting each online
  endpoint's latest lat/lon + a telemetry popover. New nav entry.
- DTCs surfaced in cloud-ui (persistent `vehicle.dtc`).

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

1. `modules/vehicle/obd/pid_decode` + gtest (pure, no hardware) — lands the
   decode logic testable in CI first.
2. `iot-vehicled` daemon + `vehicle.lua` + units + ServiceGate (ingest →
   volatile ds). device-ui Vehicle page.
3. Vehicle LwM2M object + reader hooks (device) → cloud observe → cloud ds.
4. cloud-ui Map page (reuses GPS Object 6 + vehicle volatile keys).
5. DTC path (ISO-TP Mode 03 → `vehicle.dtc` → UI).
6. `iot-mqttd` + MQTT config page + Yocto `libmosquitto`.

## 9. Open items to confirm before coding

- **CAN controller hardware** for the target (RPi MCP2515 HAT vs USB-CAN) — sets
  the kernel module + `iot-can0-up` details.
- **Bus speed**: 500 kbps (ISO 15765-4 11-bit) vs 250 kbps — make it the
  `vehicle.can.bitrate` default but confirm the target vehicle.
- **Custom LwM2M object ID** for vehicle telemetry (pick from the reusable
  range; document in `apps/docs/lwm2m-object-handling.md`).
- **MQTT payload schema** (one JSON blob of all signals vs per-signal topics).
  Default: one retained JSON message per poll on `<serial>/<suffix>`.
- **TLS to broker** scope for v1 (plain 1883 first, 8883/CA later?).
