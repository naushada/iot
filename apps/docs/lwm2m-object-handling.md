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

## 5. File map

| Concern | File |
|---------|------|
| Bootstrap server (`/bs`, TLV encode, resolver hook) | `apps/src/lwm2m_bootstrap_server.cpp`, `apps/inc/lwm2m_bootstrap_server.hpp` |
| Bootstrap client (writes, commit, DTLS install) | `apps/src/lwm2m_bootstrap_client.cpp` |
| Shared Security/Server instance types | `apps/inc/lwm2m_bootstrap.hpp` |
| Cloud-BS resolver wiring (data-store driven) | `apps/src/main.cpp` (`wire_server` resolver) |
| Client object install | `apps/src/lwm2m_object_stubs.cpp` |
| Registration server + registry (lifetime timer) | `apps/src/lwm2m_registration_server.cpp`, `apps/src/lwm2m_registration.cpp` |
| Online/offline publish (lwm2m-dm) + merge (iot-cloudd) | `apps/src/main.cpp` (`wire_server`), `apps/cloud/server/src/main.cpp` (`reconcile_registrations`); key `cloud.lwm2m.registrations` |
| Registration client (`/rd`, Update) | `apps/src/lwm2m_registration_client.cpp` |
| PSK provisioning (mint DM PSK) | `apps/cloud/server/src/main.cpp`, `apps/src/psk_gen.cpp` |
| Bootstrap/DM provisioning source | data-store: `cloud.endpoint.credentials`, `cloud.bs.uri`, `cloud.dm.uri/lifetime/binding` (no static lua) |
