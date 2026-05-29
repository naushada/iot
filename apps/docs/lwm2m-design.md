# OMA LwM2M Implementation Design

Target spec: **OMA LwM2M 1.1** (`OMA-TS-LightweightM2M_Core-V1_1_1` +
`OMA-TS-LightweightM2M_Transport-V1_1_1`).
References to LwM2M 1.0 noted where the wire format differs.

This document is an implementation plan — what we have today in
`apps/src/lwm2m_adapter.cpp`, what the spec mandates, and the gap between
them. It assumes the ACE refactor (see `ace-refactor.md`) has landed and
that the I/O layer is no longer in scope.

## 1. Scope

In scope:

- LwM2M roles we will implement: **Bootstrap Server**, **LwM2M Server**
  (device-management), **LwM2M Client**. Same binary, role selected at CLI.
- Four LwM2M interfaces: Bootstrap, Registration, Device Management &
  Service Enablement, Information Reporting.
- Payload encodings: **TLV** (mandatory), **plain text**, **opaque**, **JSON**,
  **SenML JSON**, **SenML CBOR**. Plain CBOR is optional in 1.1; we will
  add it because the codebase already pulls in `nlohmann::json` CBOR.
- Security: **PSK over DTLS 1.2** (the existing tinydtls path). RPK / X.509
  are out of scope for v1.

Out of scope (deferred):

- LwM2M over MQTT / HTTP / Non-IP / LoRaWAN.
- Firmware update binary delivery (object 5 resource 0 — only the model is
  represented).
- Access Control enforcement (object 2 — parsed but not enforced).
- SMS binding.
- LwM2M 1.2 features (CoAP-over-TCP, SenML CBOR composite resources).

## 2. Reference: what the four interfaces have to do

| Interface | Op | CoAP method | URI | Required by |
|-----------|----|-------------|-----|-------------|
| Bootstrap | Request | POST | `/bs?ep={ep}` | Client → BS |
|           | Write   | PUT  | `/{oid}` or `/{oid}/{iid}` | BS → Client |
|           | Delete  | DELETE | `/` or `/{oid}/{iid}` | BS → Client |
|           | Discover | GET | `/{oid}` (Accept: link-format) | BS → Client |
|           | Finish  | POST | `/bs` | BS → Client |
| Registration | Register | POST | `/rd?ep={ep}&lt={s}&lwm2m=1.1&b=U` | Client → Server |
|           | Update  | POST | `/rd/{location}?lt=…&b=…` | Client → Server |
|           | Deregister | DELETE | `/rd/{location}` | Client → Server |
| Device Management & Service Enablement | Read | GET | `/{oid}[/{iid}[/{rid}[/{riid}]]]` | Server → Client |
|           | Discover | GET (Accept: link-format) | `/{oid}…` | Server → Client |
|           | Write   | PUT or POST | `/{oid}/{iid}[/{rid}]` | Server → Client |
|           | Create  | POST | `/{oid}` | Server → Client |
|           | Delete  | DELETE | `/{oid}/{iid}` | Server → Client |
|           | Execute | POST | `/{oid}/{iid}/{rid}` | Server → Client |
|           | Write-Attributes | PUT | `/{oid}/…?pmin=…&pmax=…&gt=…&lt=…&st=…` | Server → Client |
| Information Reporting | Observe | GET + Observe: 0 | `/{oid}/…` | Server → Client |
|           | Notify  | NON 2.05 + Observe: seq | (response to observed URI) | Client → Server |
|           | Cancel  | GET + Observe: 1 *or* RST | `/{oid}/…` | Server → Client |

These are the operations we need to implement; everything else flows from
them.

## 3. Object model

Mandatory objects (must be present on every client):

| OID | Name | Mandatory in spec | Today's code |
|----:|------|:-----------------:|:------------:|
| 0   | Security | yes | RID map ✓, TLV encode ✓, JSON config ✓ |
| 1   | Server   | yes | RID map ✓, TLV encode ✓, JSON config ✓ |
| 3   | Device   | yes | RID map ✓, no live values |

Common objects we will model:

| OID | Name | Today |
|----:|------|------|
| 2   | Access Control | RID map ✓, no enforcement |
| 4   | Connectivity Monitoring | RID map ✓ |
| 5   | Firmware Update | RID map ✓ |
| 6   | Location | RID map ✓ |
| 7   | Connectivity Statistics | RID map ✓ |

`LwM2MAdapter` already carries the `<rid → name>` and `<name → rid>` maps
for all of the above (`apps/src/lwm2m_adapter.cpp:8-244`). What's missing
is the **runtime resource store**: today the only "values" are the JSON
files under `apps/config/{security,server}Object/*.json`, read at bootstrap
time and pushed once. There is no resource read/write/observe on live
state.

### 3.1 Internal representation we will introduce

```cpp
struct Resource {
    std::uint32_t              rid;
    enum class Type {
        String, Integer, Float, Boolean, Opaque, Time, ObjLink, None
    } type;
    bool                       multiple;     // resource instance vs single
    bool                       observable;
    std::function<std::string()> read;       // returns wire bytes (TLV item value)
    std::function<int(const std::string&)> write;  // returns 0 / error code
    std::function<int()>       execute;      // for executable resources
    // observer bookkeeping:
    std::vector<ObserveCtx>    observers;
};

struct ObjectInstance {
    std::uint32_t              iid;
    std::unordered_map<std::uint32_t, Resource> resources;
};

struct ObjectDescriptor {
    std::uint32_t              oid;
    std::string                name;
    bool                       multipleInstance;
    bool                       mandatory;
    std::map<std::uint32_t, ObjectInstance> instances;
};

class ObjectStore {
    std::map<std::uint32_t, ObjectDescriptor> m_objects;
    // CRUD, lookup by oid/iid/rid/riid, link-format discovery output
};
```

`LwM2MAdapter` becomes the protocol facade: it takes a parsed
`CoAPMessage`, looks up the addressed resource in the `ObjectStore`, calls
the right callback, and serialises the result.

`Resource::read`/`write`/`execute` give us a single attachment point per
RID — Device object resource 17 ("Device Type") binds to a string literal;
resource 13 ("Current Time") binds to `time(nullptr)`; resource 9 ("Battery
Level") binds to a platform reader. The bootstrap JSON files become **just
one of many** sources for these callbacks.

## 4. Payload encodings

### 4.1 TLV (mandatory)

The encoding is in `LwM2MAdapter::serialiseTLV`/`parseLwM2MObjects` today;
both work for the bootstrap path. What we need on top:

- **`parseLwM2MObjects` is recursive and uses an internal istringstream**.
  When the TLV nesting is deeper than two levels (Object Instance →
  Multiple Resource → Resource Instance) the recursion drops data on the
  first error. We'll rewrite with an explicit cursor + bounded reads.
- **Multiple-resource decoding** (TLV type bits `10`) is partial. The
  enum value is defined; the parse path for it falls through without
  populating `m_riid` for each inner instance.
- **Boolean serialise** emits 0x00/0x01 in one byte (correct); **time**
  resources are missing — encode as int64 big-endian per Annex C.

### 4.2 SenML JSON / SenML CBOR

LwM2M 1.1 made SenML the default text payload (the older custom JSON
remains for backward compatibility). We need:

- Encoder: take an `ObjectInstance` (or a `Resource`) and emit the SenML
  RFC 8428 record array. Base name (`bn`) = `/oid/iid`; `n` = relative
  path; value field chosen by type (`v` int/float, `vs` string, `vb`
  boolean, `vd` opaque base64, `vlo` ObjLink).
- Decoder: parse SenML records into `LwM2MObject`s using the existing
  data structures.
- Content-Format codes: `senml+json` = 110, `senml+cbor` = 112 (the
  current code only knows 11542 LwM2M+TLV and 11543 LwM2M+JSON).
- CBOR path reuses `nlohmann::json::to_cbor` / `from_cbor` so we don't
  add a second CBOR library.

### 4.3 Plain text / opaque

Single-resource reads/writes against `/{oid}/{iid}/{rid}`. Spec mandates
plain text for numeric / string types and opaque for binary. We will add
a thin one-shot encoder that dispatches by `Resource::type`.

### 4.4 link-format (RFC 6690)

Required for Discover and for the Register payload. Format is
`</1/0>;ver="1.1",</3/0>,</5/0>` etc. We need a small printer that walks
the `ObjectStore` and emits the registered set, honoring the spec's
attribute syntax for per-resource `pmin`, `pmax`, `gt`, `lt`, `st`.

## 5. State machines

### 5.1 Bootstrap Client (replaces `App::LwM2MBootstrapState`)

```
IDLE
  │ (client started, BS configured)
  ▼
SEND_BS_REQUEST              ──► POST /bs?ep=… (Confirmable)
  │ 2.04 Changed
  ▼
WAIT_FOR_BS_WRITES           ── PUT  /0/{iid}  (Security)
  │                          ── PUT  /1/{iid}  (Server)
  │                          ── DELETE /{oid}/{iid}  (optional purge)
  │ idle ≥ T_bs_purge (default 247s per spec)
  │ OR
  │ BS sends POST /bs (Bootstrap-Finish)
  ▼
APPLY_BOOTSTRAP              (commit Security & Server objects to store)
  ▼
REGISTER                     (triggers the Registration FSM below)
```

Today, the client's bootstrap path consists of typing `post uri="/bs"` at
the readline prompt. Driving this from `App` requires:

- A `BootstrapClient` class that owns this FSM and is fed by
  `CoAPAdapter::processRequest` decisions.
- A "bootstrap pending writes" buffer so `apply` is atomic — partial
  writes are discarded on `/bs` finish failure.

### 5.2 Bootstrap Server (replaces today's static reply in `processRequest`)

```
LISTEN
  │  POST /bs?ep=…  +  DTLS PSK by ep
  ▼
SEND_SECURITY_INSTANCES      ──► PUT /0/0 (this BS account),  PUT /0/1 (DM)
  │
  ▼
SEND_SERVER_INSTANCES        ──► PUT /1/{iid}
  ▼
(optional) DELETE other      ──► DELETE /3, /4, etc.
  ▼
FINISH                       ──► POST /bs
  ▼
LISTEN
```

The existing `buildLwM2MPayload` produces TLV for OID 0 and OID 1; what's
missing is **issuing them as separate CoAP requests with the right URI**
instead of a single reply blob. This is the change that turns the current
"reply with a TLV blob" into a real BS interaction.

### 5.3 Registration Client

```
UNREGISTERED
  │ (bootstrap complete; Server object lifetime = LT)
  ▼
SEND_REGISTER                ──► POST /rd?ep=…&lt=LT&lwm2m=1.1&b=U
  │                            payload: link-format (/1/0, /3/0, …)
  │ 2.01 Created  Location-Path: /rd/{loc}
  ▼
REGISTERED                   (start lifetime timer = LT - 30s margin)
  │  on timer expiry, or on observable-resource changed-and-buffered
  ▼
SEND_UPDATE                  ──► POST /rd/{loc}?lt=LT [payload: link-format if objects changed]
  │ 2.04 Changed
  ▼
REGISTERED
  │ (shutdown)
  ▼
SEND_DEREGISTER              ──► DELETE /rd/{loc}
  ▼
UNREGISTERED
```

The reactor's idle timeout (1 s in the post-ACE code) is the natural place
to advance the lifetime ticker. On expiry, schedule a registration update;
the ACE handler `UDPAdapter::handle_exception` (the notify drain) then
sends the CoAP frame.

### 5.4 Registration Server

```
LISTEN
  │ POST /rd?ep=…&lt=…
  ▼
PARSE_LINK_FORMAT (extract advertised object instances)
  │
  ▼
ALLOCATE_LOCATION ({loc} = random/incrementing)
  ▼
STORE_REGISTRATION (ep, loc, lt, sms, b, advertised set, peer addr, expires_at)
  ▼
REPLY 2.01 + Location-Path: /rd/{loc}
  ▼
LISTEN

(on POST /rd/{loc})     → lookup, refresh expires_at, optional payload merge → 2.04
(on DELETE /rd/{loc})   → drop → 2.02
(on expires_at passed)  → drop → fire "ClientGone" hook
```

Today, the server replies 2.04 to `/rd` without recording the registration
or generating a `Location-Path`. The new `ClientRegistry` owns this state.

### 5.5 Information Reporting

```
OBSERVE_RECEIVED (GET /uri  + Observe: 0  + Token)
   ▼
ADD_OBSERVER (uri, peer, token, pmin, pmax, gt, lt, st)
   │
   ▼
REPLY 2.05 + Observe: seq=0 + payload (current value)

(on resource change)
   if  now - last_notify < pmin → defer
   elif now - last_notify ≥ pmax OR value crossed threshold → NOTIFY
   ▼
NOTIFY (NON 2.05 + Observe: seq++ + payload)

(on RST or Observe: 1 GET)
   ▼
REMOVE_OBSERVER
```

The observer table lives next to the `Resource`. Threshold logic uses
`gt`, `lt`, `st` (greater-than, less-than, step) as defined in §6.3.6.

## 6. Security mapping

The Security Object (OID 0) drives DTLS credentials. Per-instance:

| RID | Field | DTLS effect |
|----:|-------|-------------|
| 0   | LwM2M Server URI | `host:port` to connect to |
| 1   | Bootstrap-Server | true → instance is BS account |
| 2   | Security Mode | 0=PSK, 1=RPK, 2=Cert, 3=NoSec, 4=CertWithEST |
| 3   | Public Key or Identity | PSK identity (or RPK/cert bytes) |
| 5   | Secret Key | PSK key (or RPK private) |
| 10  | Short Server ID | binds this Security Object to a Server Object |

The ACE refactor's `DTLSAdapter::add_credential(identity, secret)` is the
existing hook; we will drive it from the Security Object commit step at
the end of bootstrap, rather than from the CLI.

NoSec (`SecurityMode == 3`) means the LwM2M client talks plain CoAP on
port 5683; this path already works through `Scheme_t::CoAP`. The
`Security` object then determines which `UdpService` instance to use.

## 7. Gap analysis vs current code

| Capability | Current code | Gap |
|-----------|--------------|-----|
| TLV serialise (write side) | `serialiseTLV` + `serialiseObjects` | Time type missing; multiple-resource path only emits the inner blob |
| TLV parse (read side) | `parseLwM2MObjects` recursive | Deep nesting fragile; no `m_riid` propagation for multi-resources |
| SenML JSON / CBOR | none | Add encoder + decoder; new content-formats 110/112 |
| link-format | none | Required for Register payload + Discover responses |
| Bootstrap Client FSM | typed via readline | Move into `BootstrapClient`; auto-trigger on connect |
| Bootstrap Server FSM | reply with TLV blob at `/bs` | Issue per-object PUTs + send `POST /bs` finish |
| Register payload | no payload built | Emit link-format of installed objects |
| Register Location | server returns 2.04, no Location | Allocate `{loc}`, return as Location-Path option |
| Lifetime ticker | none | Reactor-idle hook drives Update + expiry |
| Read / Write / Execute | not implemented | `ObjectStore` lookup + Resource callbacks |
| Discover (link-format response) | not implemented | Required for `?` query and BS Discover |
| Observe / Notify | not implemented | New observer table per Resource; threshold engine |
| Access Control enforcement | none | Out of scope v1 |
| Object 3 (Device) live values | **L8 done** — `apps/inc/lwm2m_object_3_device.hpp::install_device` binds constants + JSON overrides + Linux readers per D6 | — |

## 8. Code layout after this work

```
apps/inc/
  lwm2m_adapter.hpp          # protocol facade (slimmed)
  lwm2m_object_store.hpp     # new: ObjectStore, ObjectDescriptor, Resource
  lwm2m_codec_tlv.hpp        # new: TLV encode/decode (carved out of lwm2m_adapter)
  lwm2m_codec_senml.hpp      # new: SenML JSON / CBOR
  lwm2m_codec_linkformat.hpp # new: link-format printer/parser
  lwm2m_bootstrap_client.hpp # new: BootstrapClient FSM
  lwm2m_bootstrap_server.hpp # new: BootstrapServer FSM
  lwm2m_registration_client.hpp # new
  lwm2m_registration_server.hpp # new (ClientRegistry)
  lwm2m_observer.hpp         # new: observer table, threshold engine
  lwm2m_resources/
    object_0_security.hpp    # bindings for OID 0
    object_1_server.hpp
    object_3_device.hpp
    …
apps/src/
  …matching .cpp files…
```

`coap_adapter.cpp::processRequest` shrinks: it parses CoAP, classifies the
URI (`/bs`, `/rd`, `/rd/{loc}`, `/{oid}/...`), and dispatches to one of
the FSMs above. The big content-format `if/else` ladder for uCBOR /
SUCBOR / etc. moves into a `PayloadCodec` registry keyed by content-format
code.

## 9. Wire compliance checklist (what we'll prove)

After implementation, run against a known LwM2M test harness — the
canonical free choice is Leshan
(<https://github.com/eclipse-leshan/leshan>) — in two configurations:

1. Our client ↔ Leshan server (Bootstrap + Registration + a read/write
   round-trip on `/3/0/13`).
2. Leshan client ↔ our server (same coverage, opposite direction).

Both PSK and NoSec. Capture pcaps and diff against the existing
`log/dtlsc.txt` / `log/dtlss.txt` to confirm the DTLS handshake didn't
change.

**L9 status**: the concrete test plan with the exact CLI invocations,
expected outcomes, BUG-001 / BUG-002 runtime regression cases, and
NFR-PERF-002 / NFR-PERF-003 sampling notes is at
`apps/docs/leshan-interop.md`. The `docker/docker-compose.leshan.yml`
harness brings up both interop matrices in two containers each.
App-level wiring lives in `apps/src/main.cpp::wire_server` and
`wire_client`. The remaining runtime gaps (Register/Update send,
notify shipping from the tick path) are itemised under §8 of the
interop doc — all < 50 LOC each.

## 10. Implementation phases (proposed)

Each phase is a separate PR, sized so reviewers can hold it in head.

| Phase | Adds | LOC est. |
|------:|------|---------:|
| L1 | `ObjectStore` + `lwm2m_codec_tlv` (carved out, no behavior change) | 600 |
| L2 | link-format printer + Discover response | 300 |
| L3 | Registration Client + Server (uses link-format) | 600 |
| L4 | Bootstrap Client + Server FSMs (replace the readline-driven path) | 700 |
| L5 | Read / Write / Execute via `Resource` callbacks | 500 |
| L6 | SenML JSON + SenML CBOR codecs (content-formats 110 / 112) | 500 |
| L7 | Observe / Notify with threshold engine | 600 |
| L8 | Device object live bindings + Connectivity Monitoring | 300 |
| L9 | Leshan interop pass (pcap diffs, bug fixes) | 200 |

L1 + L2 are pure refactors and can land in parallel with the ACE refactor
finalization. L3 onward needs the ACE refactor's reactor idle hook for
lifetime ticks.

## 11. Open questions for the next session

None. The six questions raised during L1–L7 design are closed in §12.

## 12. Decisions

- **2026-05-29 — Spec version**: pinned to **OMA LwM2M 1.1.1**.
  LwM2M 1.0 is **not** supported (no compatibility shim, no
  `lwm2m=1.0` accepted on Register). 1.2 features remain out of scope
  per §1. See `lwm2m-rdd.md` §11 D1.

- **2026-05-29 — Topology**: v1 ships **single-server** post-Bootstrap
  (one DTLS session, one registration), but all per-server data
  structures (`ServerRegistration`, `ObserveContext`,
  `ResourceAttributes`) are **keyed by Short Server ID** from day one.
  The §5.3 / §5.4 FSMs treat the count-of-Server-Object-instances as
  `1` for v1; the multi-server upgrade is then a localized change in
  L3 + L7. See `lwm2m-rdd.md` §11 D2 / REQ-REG-011.

- **2026-05-29 — Registry storage**: server-side `ClientRegistry` is
  **in-memory authoritative + async MongoDB mirror**. A dedicated
  `ACE_Task<ACE_MT_SYNCH>` worker drains a bounded
  `ACE_Message_Queue` of `RegistryEvent` records and writes them to
  Mongo via `DbClient`. The reactor enqueues and returns — it never
  blocks on DB I/O. On server restart, the registry is reconstructed
  from Mongo before opening the listening socket; if Mongo is
  unreachable at boot the server starts with an empty registry and
  logs a warning. See `lwm2m-rdd.md` §11 D3 / REQ-REG-010.

- **2026-05-29 — Notify transport**: Notify defaults to **Non-Confirmable**
  (matches Leshan, lowest per-notify overhead). The Observe engine
  emits CoAP CON in two cases: every `kConDeadPeerInterval`-th notify
  per observer (default 10) so the server can detect a dead peer
  without waiting for `pmax` skew; and on an opt-in
  `Resource::observeCritical` flag reserved for L9 tuning. RST handling
  and Observe: 1 cancel are unaffected by this policy. See
  `lwm2m-rdd.md` §11 D4 / REQ-IR-002 / NFR-PERF-003.

- **2026-05-29 — Push plane**: the existing `/push`, `/set`, `/get`,
  `/execute` custom data plane is **always compiled in**. No CMake
  flag. Coexists with the LwM2M numeric-URI plane via URI-keyword
  routing in `CoAPAdapter::processRequest`. See `lwm2m-rdd.md` §11 D5 /
  REQ-PUSH-001..003.

- **2026-05-29 — Device Object backing**: OID 3 reads from
  `apps/config/deviceObject/0.json` on startup with per-RID fallback
  to compiled-in constants. Memory + Time RIDs bind to Linux platform
  readers regardless of JSON. Mirrors the existing security /
  server-object config pattern. See `lwm2m-rdd.md` §11 D6 /
  REQ-OBJ-003 / REQ-OBJ-005.
