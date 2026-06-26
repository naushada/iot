# LwM2M Object Handling — Bootstrap & Registration

Target spec: **OMA LwM2M 1.1.1** (Core + Transport). This document describes
how the stack handles the Security Object (OID 0) and Server Object (OID 1)
during the **Bootstrap** interface, how the DM credential reaches the client,
and how the **Registration** interface tracks endpoint liveness with a
lifetime timer.

Companion docs: `lwm2m-design.md` (overall design), `lwm2m-rdd.md`
(requirements), `tdd-psk-provisioning.md` (PSK provisioning),
`../cloud/CLAUDE.md` (cloud topology + data-store keys).

---

## 1. Roles and topology

The same `lwm2m` binary runs in three roles (see `apps/cloud/CLAUDE.md`):

| Role | Container | Port | Handles |
|------|-----------|------|---------|
| Bootstrap server | `lwm2m-bs` | 5684/udp | `POST /bs` |
| Device-Management server | `lwm2m-dm` | 5683/udp | `POST /rd`, `POST /rd/{loc}`, `DELETE /rd/{loc}` |
| Client | device | — | sends `/bs` then `/rd`, applies Bootstrap-Writes |

CoAP dispatch lives in `CoAPAdapter::processRequest` (`coap_adapter.cpp`),
which routes by URI: `/bs` → Bootstrap server/client, `/rd*` → Registration
server/client.

---

## 2. Bootstrap interface — `POST /bs`

### 2.1 Wire flow

```
client                              bootstrap server (lwm2m-bs)
  │  POST /bs?ep=<endpoint>   (CON)
  │ ─────────────────────────────────▶
  │                                    look up provisioning for <endpoint>
  │  2.04 Changed             (ACK)
  │ ◀─────────────────────────────────
  │  PUT /0/0  (TLV, BS Security inst) NON
  │ ◀─────────────────────────────────
  │  PUT /0/1  (TLV, DM Security inst) NON
  │ ◀─────────────────────────────────
  │  PUT /1/0  (TLV, Server Object)   NON
  │ ◀─────────────────────────────────
  │  POST /bs  (Bootstrap-Finish)     NON
  │ ◀─────────────────────────────────
  │  2.04 Changed (ack each write / finish)
  │ ─────────────────────────────────▶
```

Server logic: `lwm2m::bootstrap::Server::handle`
(`lwm2m_bootstrap_server.cpp`). Unknown endpoint → `4.04 Not Found`;
non-POST → `4.05`; missing `ep=` → `4.00`. v1 emits the Bootstrap-Write set
as **NON** (no retransmit queue) — allowed by Core §6.1.5.

Client logic: `lwm2m::bootstrap::Client::handle_bs_traffic`
(`lwm2m_bootstrap_client.cpp`). The staged writes commit atomically on
Bootstrap-Finish in `apply_commit`.

### 2.2 Security Object (OID 0) — two instances

The Bootstrap server writes **two Security Object instances** — its own
Bootstrap-Server account and the DM-Server account (standard LwM2M layout):

**`/0/0` — Bootstrap-Server account** (`Is Bootstrap = true`):

| Resource | RID | Value |
|----------|-----|-------|
| LwM2M Server URI | 0 | `cloud.bs.uri` (e.g. `coaps://<bs-host>:5684`) |
| Bootstrap-Server | 1 | `true` |
| Security Mode | 2 | `0` (PSK) |
| Public Key or Identity | 3 | BS DTLS identity = `sha256(endpoint)[:32]` |
| Secret Key | 5 | BS PSK key, **hex** (`bs.psk.key`) |
| Short Server ID | 10 | `0` (**ignored** for a Bootstrap-Server account) |

**`/0/1` — DM-Server account** (`Is Bootstrap = false`):

| Resource | RID | Value |
|----------|-----|-------|
| LwM2M Server URI | 0 | `cloud.dm.uri` (e.g. `coaps://<dm-host>:5683`) |
| Bootstrap-Server | 1 | `false` |
| Security Mode | 2 | `0` (PSK) |
| Public Key or Identity | 3 | DM PSK identity (`dm.psk.id`) |
| Secret Key | 5 | DM PSK key, **hex** (`dm.psk.key`) |
| Short Server ID | 10 | `101` (links to Server Object `/1/101`) |

TLV encoding: `encode_security_tlv` (RIDs 3/5 omitted when empty). The BS
identity is *derived*, not stored — the device and `register_endpoint_creds`
both compute `sha256(endpoint)[:32]`. The BS instance is written only when
`cloud.bs.uri` is set.

### 2.3 Server Object (OID 1)

Paired DM-server account. The **instance id equals the Short Server ID**, so
the Security RID 10 (`101`) links directly to `/1/101`:

| Resource | RID | Value |
|----------|-----|-------|
| Short Server ID | 0 | `101` (== Security `/0/1/10`) |
| Lifetime | 1 | `cloud.dm.lifetime` (default 86400 s) |
| Binding | 7 | `cloud.dm.binding` (default `U`) |

`dmSsid` is currently a constant (`101`) in the resolver — v1 is a single DM
server, so one SSID. Both the Security RID 10 and the Server Object instance
id + RID 0 use it.

### 2.4 Where the DM PSK comes from (important)

The DM PSK is **not generated at `/bs`**. It is minted **once per endpoint,
at provisioning time, by `iot-cloudd`** (`apps/cloud/server/src/main.cpp`,
on `cloud.provision.request`): `generate_psk_hex()` → `upsert_credential` →
`cloud.endpoint.credentials`. The DM server (`lwm2m-dm`) loads those same
creds at startup (`register_endpoint_creds` in `main.cpp`) and validates the
client's DTLS handshake against them.

So the Bootstrap server must hand the client the **same** credential the DM
already trusts — re-minting at `/bs` would give the client a key the DM never
loaded, and the post-bootstrap DTLS to the DM would fail.

`cloud.endpoint.credentials` record (JSON array, keyed by serial / formatted
identity):

```json
[{
  "serial":     "100000abcd",
  "identity":   "rpi100000abcd@cloud.local",
  "bs.psk.key": "<hex>",
  "dm.psk.id":  "rpi100000abcd@cloud.local",
  "dm.psk.key": "<hex>"
}]
```

The DM identity (`rpi<serial>@cloud.local`) is **distinct** from the BS DTLS
identity (`sha256(endpoint)`), so bootstrap and DM use different PSKs.

### 2.5 Live resolution at `/bs`

The cloud Bootstrap server resolves the DM account **per request** rather
than from a static config file, via the hook
`Server::provisioning_resolver` (`lwm2m_bootstrap_server.hpp`). `handle`
prefers the resolver and falls back to the static `add_account()` map (the
device-side / unit-test path is unchanged).

Wiring: `wire_server` in `main.cpp` installs the resolver only when
`lwm2m-instance=bs`. The resolver reads `cloud.endpoint.credentials`,
`cloud.dm.uri`, `cloud.dm.lifetime`, `cloud.dm.binding`, `cloud.bs.uri` from
the data-store, matches the requesting endpoint by `serial` or `identity`,
and builds the Security `/0/0` (BS) + `/0/1` (DM) + Server `/1/0` instances
above. No provisioned record (or unset `cloud.dm.uri`) → resolver returns
nothing → `4.04 Not Found`.

> The static Security config files (`apps/config/securityObject/*.lua`) were
> **removed entirely** — Bootstrap provisioning is 100% data-store driven.
> The server-side loader (`load_provisioning_from_config`) and the static
> placeholder account were deleted; `wire_server` relies solely on the
> resolver. The client-side installer (`install_security`) still honours a
> file if one exists, but advertises nothing until bootstrap populates the
> object.

### 2.6 PSK key representation (hex end-to-end)

PSK secrets are carried and stored as **hex strings**; the DTLS PSK callback
hex-decodes them (`dtls_adapter.cpp` — "Secrets are stored as hex strings").
This is consistent on every leg:

- Bootstrap server puts the hex `dm.psk.key` in RID 5.
- Client decodes RID 5 to the hex string and calls
  `DTLSAdapter::add_credential(identity, hex)` at commit
  (`apply_commit`, PSK mode only).
- DM server calls `add_credential(dm.psk.id, dm.psk.key-hex)` at startup.

Both sides hex-decode the same string → the same raw key → the handshake
matches.

### 2.7 Re-provisioning a PSK at runtime (hot change)

PSK credentials are read **once at process start** and registered with DTLS
(`add_credential`). They are not re-read on every handshake, so the two sides
re-provision differently:

- **Device client — auto self-restart.** The client subscribes to its own
  `iot.bs.psk.key` (`ds.on_change`, `apps/src/main.cpp`). When the key changes
  underneath it — e.g. an engineer edits it via `ds-cli`, or the cloud
  re-provisions — `should_restart_on_psk_change` fires and the client calls
  `::exit(0)`. systemd (`Restart=always`) relaunches it, which reloads the new
  BS PSK, re-registers the DTLS credential, and re-runs bootstrap → DM with the
  fresh key. **So yes: the device LwM2M client re-initialises itself on a BS PSK
  change — no manual restart needed.** (The client never writes `iot.bs.psk.key`
  itself, so any observed change is genuinely external; the initial provisioning
  write while uninitialised does *not* trigger a restart.)
- **Cloud BS/DM server — manual restart.** The per-endpoint creds in
  `cloud.endpoint.credentials` are loaded once at startup
  (`register_endpoint_creds`); there is **no** watch re-loading them. After
  changing an endpoint's `bs.psk.key`/`dm.psk.*`, restart the cloud daemon
  (`docker restart iot-cloudd` / restart the `lwm2m-bs` unit) so it picks up the
  new credential.

### 2.8 Pushing the VPN server endpoint (Object 2048 RIDs 5/6/7)

The device needs to know *where* to dial the OpenVPN server. Rather than seed
it through docker-compose env (those seeds were removed — **ds is the source of
truth**), the cloud DM server pushes the endpoint down the same Object 2048
delivery that carries the VPN cert family.

**Cloud side** (`build_cert_push`, `apps/src/lwm2m_dm_server.cpp`). Alongside
the ca/cert/key chunks (instances 0/1/2 → RID 1), it appends three small
plain-text WRITEs to **instance 0**:

| RID | ds key derived from | Notes |
|-----|---------------------|-------|
| 5 (VPN host)  | `cloud.dm.uri` (host parsed out) | only emitted when known; empty host → cert-only push (back-compatible) |
| 6 (VPN port)  | `cloud.vpn.listen.port` | as a decimal string |
| 7 (VPN proto) | `cloud.vpn.proto`, mapped `tcp-server` → `tcp-client` | the device dials, so the role is flipped |

The push (`apps/src/main.cpp`, DM wiring) is sent on each registration/cert-push
cycle until `cloud.vpn.connected` lists the endpoint, then it stops. The push is
finalised by the same **RID 3 (Apply) EXECUTE** that commits the cert family.

**Device side** (`certHooks.set_vpn_endpoint`, `apps/src/main.cpp`). At Apply,
the staged values are materialised into `vpn.remote.host` / `vpn.remote.port`
(parsed to the schema's integer) / `vpn.remote.proto`. The cert hook's
`false→true` gate-flip on `services.openvpn.client.enable` (see §2 of
`modules/openvpn/client/docs/design.md`) then hot-reloads openvpn-client, which
re-execs reading the new `vpn.remote.*` — no compose seed, no manual restart.

**VPN host is also derived at bootstrap (derive-with-override).** Because the
VPN concentrator is co-located with the DM on the cloud VM, the VPN host equals
the DM URI host. So the device sets `vpn.remote.host` *itself* at bootstrap
commit — `DsConfig::set_vpn_remote_host(dmHost)` in the BS `on_done`
(`apps/src/main.cpp`), right after it persists `iot.dm.uri`. This means a
co-located cloud needs **zero** VPN-host config on the device (only the
commissioned bootstrap URI + BS PSK), and the device learns the host before
registration rather than waiting on the Object-2048 endpoint push. The push
(RID 5) above remains the **override**: for a split topology where the VPN
concentrator ≠ DM host, the operator-driven push wins; in the common single-VM
case both write the identical host, so the override is idempotent. Port/proto
have no on-device default and still come from the push (the cloud knows them
authoritatively).

---

## 3. Registration interface — `POST /rd`

After bootstrap commits, the client registers to the DM server:

- **Register** `POST /rd?ep=&lt=&lwm2m=1.1&b=U` with a link-format payload
  of advertised objects (`RegistrationClient::build_register_request`). The
  DM server (`RegistrationServer::handle`) adds the client to
  `ClientRegistry` and replies `2.01 Created` with a **Location-Path**
  (`/rd/{id}`) generated by `ClientRegistry::add`.
- **Update** `POST /rd/{location}` at the lifetime margin
  (`should_send_update`), refreshing lifetime/binding.
- **Deregister** `DELETE /rd/{location}` → `2.02 Deleted`.

The `/rd/{location}` is assigned by the **DM server at Register time** (not by
the Bootstrap server) and is what the client uses for periodic Update.

---

## 4. Lifetime timer + online/offline endpoint state

Each registration carries a **lifetime timer**:

| Event | Registry action | Endpoint state |
|-------|-----------------|----------------|
| Register `POST /rd` | `add()` sets `expiresAt = now + lifetime` | **online** |
| Update `POST /rd/{loc}` | `update()` refreshes `expiresAt = now + lifetime` (restarts the timer) | stays **online** |
| Lifetime elapsed (no Update) | `expire(now)` evicts where `now >= expiresAt` | **offline** |
| Deregister `DELETE /rd/{loc}` | entry removed | **offline** |

The registry primitives (`lwm2m_registration.cpp`: `add` / `update` /
`expire`; `ServerRegistration::expired`) drive it. The 1 Hz server tick
(`wire_server` → `on_tick_server`) calls `registry->expire(now)` every second,
so an Update *is* the timer restart and a missed Update lets the entry lapse.

**Publishing the state** is wired (`wire_server`, DM instance only):

1. lwm2m-dm is the **sole writer** of `cloud.lwm2m.registrations` — a JSON
   array `[{ endpoint, registered, last_seen_unix }]` of the currently-
   registered set. It is republished on every `RegistrationServer::on_event`
   (Register / Update / Deregister) and whenever `registry->expire()` drops an
   entry in the tick. Only the DM instance publishes (the BS never sees /rd),
   so there is no empty-set clobber from the BS.
2. **iot-cloudd** watches `cloud.lwm2m.registrations`, folds it into its
   `EndpointRegistry` (`update_state(ep, online)`), and re-emits
   `cloud.endpoints` with the corrected `state` / `registered`. A dedicated
   key (rather than lwm2m-dm writing `cloud.endpoints` directly) avoids a
   two-writer clobber on the `tun_ip` / `proxy_port` fields iot-cloudd owns.

Endpoint identity is the raw serial on both sides (the device registers with
`ep=<serial>`; iot-cloudd keys its registry by the provisioned serial), so the
merge matches by exact endpoint string.

> Spec nuance: clients send Update slightly *before* `lt` elapses, so a small
> grace window beyond `expiresAt` avoids false "offline" flaps. Marking
> offline exactly at `expiresAt` is acceptable for v1.

---

## 5. Server-initiated Reads (firmware version, LAN IP, vehicle telemetry)

Beyond Register/Update the DM server also **reads** device resources so the
cloud can show what the device is running, where it lives, and — for vehicle
gateways — its live GPS position + OBD-II telemetry:

| Resource | Object/RID | Tag | Device source | Cloud sink |
|----------|------------|-----|---------------|-----------|
| Firmware version | `/3/0/3` | `0x06` | `iot.version` | `cloud.lwm2m.registrations.installed_version` |
| IP Addresses | `/4/0/4` | `0x07` | `net.iface.active.ip` | `cloud.lwm2m.registrations.lan_ip` |
| GPS latitude/longitude | `/6/0/0`, `/6/0/1` | `0x08`, `0x09` | `gps.lat`/`gps.lon` (cellular GNSS) | `cloud.vehicle.telemetry` |
| Vehicle signals + DTCs | `/33000/0/{0,1,2,3,4,5,6,7,8,10}` | `0x0A`–`0x13` | `vehicle.*` (iot-vehicled / OBD-II) | `cloud.vehicle.telemetry` |

**Flow** (`apps/src/main.cpp`, DM-side poll loop):

1. The poll loop issues a CoAP **Read** per registered endpoint, stamping each
   request's CoAP token with `[tag, seq24]` (tag selects the resource, `seq`
   correlates the async reply to its endpoint via `seqToEp`). The vehicle
   signals are driven from a single `kVehReads` table of `(tag, rid, field)`.
2. `dmResponseHandler` matches the tag and stores the payload into the right
   per-endpoint map (`epVersions`/`epLanIps`/`epGpsLat`/`epGpsLon`, or the
   nested `epVeh[endpoint][field]` for Object 33000).
3. `publish_regs` folds version/lan_ip into `cloud.lwm2m.registrations`, **and**
   writes a `cloud.vehicle.telemetry` row `{endpoint, lat, lon, speed, rpm, …}`
   (VOLATILE, latest-wins) for every endpoint that has a GPS fix.
4. **iot-cloudd** merges version/lan_ip into `cloud.endpoints`; the **cloud-ui
   Fleet Map** reads `cloud.vehicle.telemetry` directly (live markers + popups).
   A device with no GNSS/Vehicle object replies empty/4.04 → no row/field.

> Note: this server-Read path makes the **live** map work with **no LwM2M Send**
> — the cloud polls. The 60-day history pipeline (device buffer → Send → cloud
> Mongo) is separate; see `apps/docs/tdd-vehicle-telemetry.md`. The custom
> **Vehicle object (OID 33000)** is installed device-side by `install_vehicle`
> (`apps/src/lwm2m_object_stubs.cpp`) with reader hooks bound to `vehicle.*`.

The device serves `/4/0/4` live: `install_connmon` takes an `ipReader` wired in
`install_canonical_objects` to `deviceHooks.ipAddresses`, which reads
`net.iface.active.ip` from ds — net-router owns interface selection, so the LAN
IP always tracks the active WAN bearer (eth0/wlan0/wwan0) and follows DHCP
renews. The public/ISP IP shown next to it is **not** from LwM2M — iot-cloudd
derives it from the OpenVPN management `status` real-address (see
`apps/cloud/CLAUDE.md` → "Two device IP columns").

---

## 6. Server-Send payload — `POST /dp` (SenML CBOR)

`Send` (LwM2M 1.1 §6.4.6 / §8.2.5) is the **device→server push** complement to
the server-Read poll in §5: instead of the cloud Reading each resource, the client
**POSTs a batch of timestamped readings to `/dp`**. The live Fleet Map uses the
poll (§5, no Send needed); Send carries the **device-pushed / 60-day-history**
telemetry stream. The codec + framing + server decode are complete and
unit-tested; the on-device session-I/O glue is the remaining HW step (see
`apps/docs/tdd-vehicle-telemetry.md`).

### CoAP frame
| Field | Value |
|-------|-------|
| Method | `POST`, Confirmable (CON) |
| Uri-Path | `dp` |
| Content-Format | **112** = `application/senml+cbor` (`CF_SENML_CBOR`); SenML JSON `110` also decodes server-side |
| Payload | one SenML **pack** (below) |
| Response | `2.04 Changed` on accept · `4.15` wrong Content-Format · `4.00` malformed SenML · *no reply* if not `POST /dp` |

Builder: `send::build_send_request` (`apps/inc/lwm2m_send.hpp`). Receiver:
`SendServer::handle` (`apps/inc/lwm2m_send_server.hpp`) → decoded samples + ACK bytes.

### Pack = CBOR array of records
The payload is a **CBOR array** (major type 4) of **maps** (major type 5); each
map is one SenML record. Per RFC 8428 §4.5 the **first record carries the base
fields** (`bn`, `bt`) that apply to the whole pack; later records inherit them and
add only their own name/time/value. **CBOR integer labels** (RFC 8428 §6 —
`apps/src/lwm2m_codec_senml.cpp`):

| Field | Label | CBOR type | Meaning |
|-------|------:|-----------|---------|
| `bn` Base Name | **−2** | text | path prefix for all records, e.g. `"/33000/0/"` — **first record only** |
| `bt` Base Time | **−3** | int/float | Unix seconds; a record's effective time = `bt + t` — **first record only** |
| `n` Name | **0** | text | resource id appended to `bn` → `n="0"` ⇒ `/33000/0/0` |
| `t` Time | **6** | int/float | this record's offset from `bt` (0 on the first) |
| `v` Value | **2** | int/float | numeric reading |
| `vs` String Value | **3** | text | string reading |
| `vb` Bool Value | **4** | bool | boolean reading |
| `vd` Data Value | **8** | bytes | opaque reading |

Exactly **one** value field per record; unknown labels are skipped on decode
(RFC 8428 §4.1 forward-compat).

### Timestamp model (`bt` + per-record `t`)
One base time on the first record plus a small per-record offset preserves each
reading's **real capture time** across a batch — samples buffered offline upload
with their original timestamps, not arrival time. `telemetry::build_pack` /
`parse_pack` (`apps/inc/lwm2m_telemetry_pack.hpp`) convert `Sample`s ⇄ records:
`bn` = base path, `bt` = first sample's time, each record's `t` = its offset.
Consecutive records sharing an effective time **coalesce into one `Sample`** on
decode (that is how `build_pack` emits a multi-signal reading).

### Worked example
A vehicle batch — speed (`/33000/0/0`) and rpm (`/33000/0/1`) at `t=1718000000`
and again `+2 s` — shown as SenML **JSON** for readability (the wire is the CBOR
equivalent using the integer labels above):

```json
[
  {"bn":"/33000/0/", "bt":1718000000, "n":"0", "t":0, "v":62},
  {"n":"1", "t":0, "v":2150},
  {"n":"0", "t":2, "v":58},
  {"n":"1", "t":2, "v":2100}
]
```
Decodes to `/33000/0/0=62, /33000/0/1=2150` @ 1718000000 and `=58, =2100`
@ 1718000002 — two `Sample`s, four records, one base name + one base time.

---

## 7. File map

| Concern | File |
|---------|------|
| Bootstrap server (`/bs`, TLV encode, resolver hook) | `apps/src/lwm2m_bootstrap_server.cpp`, `apps/inc/lwm2m_bootstrap_server.hpp` |
| Bootstrap client (writes, commit, DTLS install) | `apps/src/lwm2m_bootstrap_client.cpp` |
| Shared Security/Server instance types | `apps/inc/lwm2m_bootstrap.hpp` |
| Cloud-BS resolver wiring (data-store driven) | `apps/src/main.cpp` (`wire_server` resolver) |
| Client object install | `apps/src/lwm2m_object_stubs.cpp` |
| Registration server + registry (lifetime timer) | `apps/src/lwm2m_registration_server.cpp`, `apps/src/lwm2m_registration.cpp` |
| Online/offline publish (lwm2m-dm) + merge (iot-cloudd) | `apps/src/main.cpp` (`wire_server`), `apps/cloud/server/src/main.cpp` (`reconcile_registrations`); key `cloud.lwm2m.registrations` |
| Server-read version (`/3/0/3`) + LAN IP (`/4/0/4`) | `apps/src/main.cpp` (DM poll loop, `dmResponseHandler`, `publish_regs`); device serves `/4/0/4` via `install_connmon` `ipReader` → `net.iface.active.ip` |
| Registration client (`/rd`, Update) | `apps/src/lwm2m_registration_client.cpp` |
| Send (`POST /dp`) frame + server decode (§6) | `apps/inc/lwm2m_send.hpp`, `apps/inc/lwm2m_send_server.hpp`, `apps/src/lwm2m_send_server.cpp` |
| SenML JSON/CBOR codec + telemetry pack (§6) | `apps/inc/lwm2m_codec_senml.hpp`, `apps/src/lwm2m_codec_senml.cpp`, `apps/inc/lwm2m_telemetry_pack.hpp` |
| Client Send upload loop (buffer → offer → emit) | `apps/inc/lwm2m_send_uploader.hpp`, `apps/inc/lwm2m_sample_buffer.hpp`, `apps/inc/lwm2m_durable_sample_buffer.hpp` |
| PSK provisioning (mint DM PSK) | `apps/cloud/server/src/main.cpp`, `apps/src/psk_gen.cpp` |
| Bootstrap/DM provisioning source | data-store: `cloud.endpoint.credentials`, `cloud.bs.uri`, `cloud.dm.uri/lifetime/binding` (no static lua) |
