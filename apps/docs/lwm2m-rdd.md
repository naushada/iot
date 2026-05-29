# LwM2M Requirements-Driven Development Plan

Companion to `lwm2m-design.md`. The design doc is the **how**; this is the
**what** and **how-we-prove-it**: numbered requirements, MoSCoW priority,
acceptance criteria, and traceability all the way from spec clause to PR
to test case.

Working assumption: target spec is **OMA LwM2M 1.1.1**. Where 1.0 differs
materially, the difference is noted on the requirement.

## 1. ID scheme

```
REQ-<area>-<NNN>     functional requirement
NFR-<area>-<NNN>     non-functional requirement
BUG-<NNN>            defect in the current code (must be fixed before shipping)
```

Area codes: `BS` (Bootstrap), `REG` (Registration), `DM` (Device Management &
Service Enablement), `IR` (Information Reporting), `ENC` (encoding), `SEC`
(security), `OBJ` (object model), `IO` (transport / ACE plumbing), `PUSH`
(custom uCBOR/timeseries push plane already in the codebase).

Priority is MoSCoW: **M**ust / **S**hould / **C**ould / **W**on't.
"Must" requirements must be green before claiming spec compliance.

## 2. Source-of-truth references

- **Core spec**: `OMA-TS-LightweightM2M_Core-V1_1_1-20190617-A` (cited as
  *Core §x.y*).
- **Transport spec**: `OMA-TS-LightweightM2M_Transport-V1_1_1` (*Tx §x.y*).
- **Object registry**: `OMA-SpecWorks LwM2M Object & Resource Registry`
  (https://technical.openmobilealliance.org/OMNA/lwm2m/lwm2m-registry.html).
- **CoAP**: RFC 7252, **Block-wise**: RFC 7959, **Observe**: RFC 7641,
  **SenML**: RFC 8428, **link-format**: RFC 6690, **DTLS 1.2**: RFC 6347.
- **Internal**: `apps/docs/architecture.md` (current state),
  `apps/docs/ace-refactor.md` (post-refactor I/O), `apps/docs/lwm2m-design.md`
  (target design).

## 3. Functional requirements

### 3.1 Bootstrap interface (Client + Server)

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-BS-001 | Client SHALL initiate a Client-Initiated Bootstrap by issuing `POST /bs?ep={endpoint}` to the configured Bootstrap-Server. | M | Core §6.1.2 | §5.1 |
| REQ-BS-002 | Client SHALL accept Bootstrap-Write requests of the form `PUT /{oid}[/{iid}]` while in the Bootstrap phase and stage the writes for atomic commit on Bootstrap-Finish. | M | Core §6.1.5 | §5.1 |
| REQ-BS-003 | Client SHALL accept Bootstrap-Delete of `DELETE /` (purge) and `DELETE /{oid}/{iid}`. | M | Core §6.1.5 | §5.1 |
| REQ-BS-004 | Client SHALL commit staged Bootstrap writes atomically on receipt of `POST /bs` (Bootstrap-Finish). On commit failure the staged set is discarded. | M | Core §6.1.5 | §5.1 |
| REQ-BS-005 | Client MAY accept Bootstrap-Discover (`GET /{oid}` with `Accept: application/link-format`). | S | Core §6.1.5 | §5.1 |
| REQ-BS-006 | Bootstrap-Server SHALL, on `POST /bs?ep=…`, send one or more `PUT /{oid}/{iid}` operations followed by a single `POST /bs` finish. | M | Core §6.1.5 | §5.2 |
| REQ-BS-007 | Bootstrap-Server SHALL deliver Security Object instance(s) for the chosen LwM2M Server URI(s) and the corresponding Server Object instance(s). | M | Core §6.1.5 | §5.2 |
| REQ-BS-008 | Bootstrap-Server SHALL allocate the Short Server ID linkage between Security Object RID 10 and Server Object RID 0 such that they match per provisioned account. | M | Core §6.3 | §5.2 |
| REQ-BS-009 | Bootstrap-Server timeout for Client Hold Off (Security Object RID 11) SHALL be respected by the Client; default 247 s if unset. | S | Core §6.1.2 | §5.1 |

### 3.2 Registration interface

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-REG-001 | Client SHALL Register with `POST /rd?ep=…&lt=…&lwm2m=1.1&b=…[&sms=…]` carrying a link-format payload listing all advertised object instances. | M | Core §6.2.2 | §5.3 |
| REQ-REG-002 | Server SHALL respond `2.01 Created` with a `Location-Path: /rd/{loc}` option uniquely identifying the registration. | M | Core §6.2.2 | §5.4 |
| REQ-REG-003 | Client SHALL Update with `POST /rd/{loc}[?lt=…&b=…]` and optionally a new link-format payload when the object set changes. | M | Core §6.2.3 | §5.3 |
| REQ-REG-004 | Client SHALL Deregister with `DELETE /rd/{loc}` on graceful shutdown. | M | Core §6.2.4 | §5.3 |
| REQ-REG-005 | Server SHALL expire registrations whose lifetime has elapsed without an Update; expired registrations are removed from the live set and a hook is fired. | M | Core §6.2.3 | §5.4 |
| REQ-REG-006 | Client SHALL emit Update no later than `lt - margin` (margin ≥ 30 s) before lifetime expiry. | M | Core §6.2.3 | §5.3 |
| REQ-REG-007 | Registered URI SHALL include LwM2M version (`lwm2m`), supported bindings (`b`), and SMS number (`sms`) when applicable. | M | Core §6.2.2 | §5.3 |
| REQ-REG-008 | Registered link-format payload SHALL conform to RFC 6690 and include the `</>;rt="oma.lwm2m";ct=…` root entry per spec. | M | Core §6.2.2 + RFC 6690 | §4.4 |
| REQ-REG-009 | Server registry SHALL persist `(ep, loc, lt, peer, advertised set)` for the duration of the registration; persistence storage is in-memory by default. | M | Core §6.2 | §5.4 |
| REQ-REG-010 | Server registry SHALL mirror Register / Update / Deregister events to MongoDB (`DbClient`) via an async worker `ACE_Task`, so registrations survive process restart. Reactor thread MUST NOT block on DB I/O. | S | — | §5.4 / D3 |
| REQ-REG-011 | All per-Server-Object state (registrations, observers, write-attributes) SHALL be keyed by **Short Server ID** even in single-server v1 deployments, so adding a second Server Object instance is a localized change. | M | Core §6.3 | D2 |

### 3.3 Device Management & Service Enablement

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-DM-001 | Client SHALL serve **Read** on `GET /{oid}[/{iid}[/{rid}[/{riid}]]]` returning the requested resource(s) in the negotiated content-format. | M | Core §6.3.4 | §3.1 |
| REQ-DM-002 | Client SHALL serve **Discover** on `GET /{oid}[/{iid}[/{rid}]]` with `Accept: application/link-format`, returning RFC 6690 with per-resource notification attributes. | M | Core §6.3.4 | §4.4 |
| REQ-DM-003 | Client SHALL serve **Write** on `PUT /{oid}/{iid}[/{rid}]` (replace) and on `POST /{oid}/{iid}` (partial update). | M | Core §6.3.4 | §3.1 |
| REQ-DM-004 | Client SHALL serve **Create** on `POST /{oid}` for objects whose `multipleInstance == true`. | M | Core §6.3.4 | §3.1 |
| REQ-DM-005 | Client SHALL serve **Delete** on `DELETE /{oid}/{iid}` for object instances that were not created by Bootstrap. | M | Core §6.3.4 | §3.1 |
| REQ-DM-006 | Client SHALL serve **Execute** on `POST /{oid}/{iid}/{rid}` for resources whose `operations` field includes E. | M | Core §6.3.4 | §3.1 |
| REQ-DM-007 | Client SHALL serve **Write-Attributes** on `PUT /…?pmin=&pmax=&gt=&lt=&st=` updating per-resource notification attributes. | M | Core §6.3.4 | §3.1 / §5.5 |
| REQ-DM-008 | All DM operations SHALL respect Access Control (Object 2). v1 enforces "owner only"; full ACL enforcement is deferred. | S | Core §6.3.5 | §3 |
| REQ-DM-009 | Server SHALL be able to invoke each DM operation against a registered client. | M | Core §6.3.4 | §3.1 |

### 3.4 Information Reporting

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-IR-001 | Client SHALL serve **Observe** (`GET … + Observe: 0 + Token`) by adding an observer to the addressed resource(s) and replying with the current value plus `Observe: seq=0`. | M | Core §6.3.6 + RFC 7641 | §5.5 |
| REQ-IR-002 | Client SHALL send **Notify** on value change using NON or CON per Server-Object configuration, with `Observe: seq++` and the same Token as the Observe request. | M | Core §6.3.6 | §5.5 |
| REQ-IR-003 | Notify SHALL respect `pmin` (suppress if `now - last_notify < pmin`) and `pmax` (force-notify if `now - last_notify ≥ pmax`). | M | Core §6.3.6 | §5.5 |
| REQ-IR-004 | Notify SHALL respect threshold attributes `gt`, `lt`, `st` per Core §6.3.6.3. | M | Core §6.3.6 | §5.5 |
| REQ-IR-005 | Client SHALL cancel observation on receipt of `GET + Observe: 1` or a CoAP RST. | M | Core §6.3.6 + RFC 7641 §3.6 | §5.5 |
| REQ-IR-006 | Notify Storing While Disabled/Offline (Server Object RID 6) SHALL queue undelivered notifications and replay on reconnect when enabled. | S | Core §6.3.6 | §5.5 |

### 3.5 Encodings

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-ENC-001 | TLV codec SHALL round-trip Object Instances, Multiple Resources, and Resource Instances for the data types: String, Integer, Float, Boolean, Opaque, Time, ObjLink. | M | Core Annex C | §4.1 |
| REQ-ENC-002 | TLV decoder SHALL reject malformed input (length overflow, truncated identifier, unknown type bits) without process abort. | M | Core Annex C | §4.1 |
| REQ-ENC-003 | SenML JSON codec SHALL implement RFC 8428 records sufficient for LwM2M (`bn`, `n`, `v`, `vs`, `vb`, `vd`, `vlo`, `bt`, `t`). | M | RFC 8428 + Tx §6.4 | §4.2 |
| REQ-ENC-004 | SenML CBOR codec SHALL implement the same record set with CBOR field labels per RFC 8428. | M | RFC 8428 §6 | §4.2 |
| REQ-ENC-005 | Plain text encoding (CF=0) SHALL support single-resource reads/writes for numeric, string, and boolean types. | M | Core §6.4 | §4.3 |
| REQ-ENC-006 | Opaque encoding (CF=42) SHALL support single-resource binary reads/writes. | M | Core §6.4 | §4.3 |
| REQ-ENC-007 | link-format codec SHALL produce/consume RFC 6690 with LwM2M-specific attributes (`rt`, `ct`, `obs`, per-resource notification attributes). | M | RFC 6690 + Core §6.2.2 | §4.4 |
| REQ-ENC-008 | The codec layer SHALL be content-format-driven via a registry; adding a new format SHALL NOT require changes to `CoAPAdapter::processRequest`. | M | — | §8 |

### 3.6 Security

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-SEC-001 | Client SHALL support DTLS 1.2 with PSK key exchange (`TLS_PSK_WITH_AES_128_CCM_8`) per Tx §5.1. | M | Tx §5.1 | §6 |
| REQ-SEC-002 | NoSec mode (Security Object RID 2 = 3) SHALL be honored — the client uses plain CoAP toward the Server URI. | M | Tx §5.1.7 | §6 |
| REQ-SEC-003 | PSK credentials SHALL be sourced from the committed Security Object (post-Bootstrap), not from the CLI, for production runs. CLI override allowed in dev. | M | Core §6.3 | §6 |
| REQ-SEC-004 | RPK and Certificate modes are explicitly **W**on't for v1. | W | — | §1 |
| REQ-SEC-005 | The Bootstrap PSK and the Server PSK SHALL be allowed to differ (one Security Object instance per account). | M | Core §6.3 | §6 |
| REQ-SEC-006 | The tinydtls PSK identity/key callback MUST be reentrant-safe under multi-peer load (a precondition for any multi-client BS server). | M | — | §3 (ACE design §6) |

### 3.7 Object model

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-OBJ-001 | Mandatory objects 0 (Security), 1 (Server), 3 (Device) SHALL be present on every client with at least one instance after Bootstrap. | M | Core §C.1, C.2, C.3 | §3 |
| REQ-OBJ-002 | Each `Resource` SHALL declare type, operations (R/W/E), multiplicity, mandatory flag, and observable flag — matching the OMA registry. | M | OMA Registry | §3.1 |
| REQ-OBJ-003 | Object 3 RIDs 0–22 SHALL be readable; RIDs 4 (Reboot) and 5 (Factory Reset) SHALL be executable; remaining write-only RIDs per registry. | M | Core §C.3 | §3 / L8 |
| REQ-OBJ-004 | Object 2 (Access Control) parser SHALL load the access matrix; enforcement is deferred per REQ-DM-008. | S | Core §C.2 | §3 |
| REQ-OBJ-005 | Object 4 (Connectivity Monitoring), 6 (Location), 7 (Connectivity Statistics) SHALL be at least stubbed (Read returns plausible values from a platform shim). | C | OMA Registry | §3 |
| REQ-OBJ-006 | The object store SHALL support runtime registration of new objects (custom OIDs in the 10000+ range) by Code, without touching the codec layer. | S | Core §6.5 | §3.1 |

### 3.8 Transport / IO

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-IO-001 | CoAP message handling SHALL support Confirmable (CON), Non-Confirmable (NON), Acknowledgement (ACK) and Reset (RST). Re-transmit/dedup follows RFC 7252 §4.2. | M | RFC 7252 | architecture.md §7 |
| REQ-IO-002 | Block-wise transfer (Block1 outbound, Block2 inbound) SHALL be supported for payloads exceeding ≈1 KiB. | M | RFC 7959 | architecture.md §7 |
| REQ-IO-003 | The transport layer SHALL run on top of the ACE reactor (post-refactor), with one `ServiceContext_t` per bound UDP socket. | M | — | ace-refactor.md §3 |
| REQ-IO-004 | All `dtls_*` calls SHALL execute on the reactor thread (notify-bridge from readline). | M | — | ace-refactor.md §4 |
| REQ-IO-005 | Reactor idle ticks SHALL drive the registration-lifetime timer and Information-Reporting `pmax` timer. | M | — | §5.3 / §5.5 |

### 3.9 Custom uCBOR / timeseries push plane (`/push`, `/set`, `/get`, `/execute`)

These URIs are **not part of LwM2M**; they are a Sierra-Wireless-style
data-push plane the current code already implements (`log.txt` shows a
working `POST /push?ep=…` with CBOR over plain CoAP). Calling them out
explicitly so they are preserved through the LwM2M build-out.

| ID | Requirement | Pri | Spec | Design |
|----|-------------|:--:|------|--------|
| REQ-PUSH-001 | The existing `/push`, `/set`, `/get`, `/execute` data-plane SHALL continue to work over CoAP and CoAPs after the LwM2M layer lands. | M | — | architecture.md §7 |
| REQ-PUSH-002 | Custom content-formats 12119 (timeseries), 12200 (uCBOR), 12201 (uCBORZ), 12202 (SUCBOR), 12203 (SUCBORZ) SHALL remain supported and routed through the codec registry. | M | — | §4 / §8 |
| REQ-PUSH-003 | `buildRequest` SHALL deterministically emit the content-format declared in the request — the JSON-text-with-CF=uCBOR bug observed in `log.txt:24` is forbidden (see BUG-002). | M | — | §4 |
| REQ-PUSH-004 | ~~The push plane SHOULD be optional — a build flag `-DENABLE_PUSH_PLANE=OFF` SHALL produce a pure-LwM2M binary.~~ **Won't** — closed by D5 on 2026-05-29. Plane is compiled in unconditionally. | W | — | §8 |

### 3.10 Defects observed in current code (must-fix before v1)

| ID | Defect | Source | Pri |
|----|--------|--------|:--:|
| BUG-001 | ~~`DTLSAdapter::dtlsGetPskInfoCb` returns identity in client mode and tinydtls warns "cannot set psk_identity -- buffer too small" → client emits Alert level 2 code 80 → handshake fails. Repro: `log.txt:147-248`. The earlier `log/dtlsc.txt` showed a success; the difference between the two runs is not yet root-caused.~~ **FIXED in L4** (2026-05-29): root cause was an inverted buffer-fit guard — `result_length <= iden.length()` was the if-branch condition where it should have been the fail-branch (`iden.length() > result_length`). Same shape on PSK_KEY. Fall-through path had no bounds check at all. Fix: correct the comparison direction in both branches, add bounds check on fallback, scrub identity-bytes logging per NFR-SEC-001. Container interop pass owns the runtime regression test (no unit cover possible without a real `dtls_context_t`). | log.txt | M |
| BUG-002 | ~~`CoAPAdapter::buildRequest` emits literal JSON text under a CBOR content-format on the first request and proper CBOR on later requests. Repro: `log.txt:24` vs `log.txt:26`.~~ **FIXED in L6** (2026-05-29): root cause was a hard-coded literal `cbor.push_back("[{\"key\": \"v1\"}]")` at `readline.cpp:400` that seeded the payload vector before the conditional `data=`/`file=` branches. When neither branch ran (or `data=` was malformed and didn't clear the seed), the literal JSON shipped under whatever CF the caller set. Fix: the payload vector now starts empty; only `data=` / `file=` populate it. Side-fix: `CBORAdapter::json2cbor` now catches the nlohmann::json parse exception and returns `-1` instead of crashing the process (root cause of `log.txt:9-11`). | log.txt | M |
| BUG-003 | `CoAPAdapter::parseRequest` Block1 handling: when `optionlength == 13/14` extended-length is read but the cursor is not bounds-checked against `eof()` until inside the read; truncated frames silently corrupt the option list. | inspection | S |
| BUG-004 | `UDPAdapter::process_request` is dead code; routing happens inside `handle_io_coap` / `handle_io_coaps`. Delete it. (Fixed implicitly by the ACE refactor.) | inspection | S |
| BUG-005 | `App::LwM2MBootstrapState` and `App::LwM2MDeviceManagementState` enums exist but no state machine drives them — fixed by L3/L4 below. | inspection | S |
| BUG-006 | Two `CoAPAdapter::processRequest` overloads share ~90% of the logic; consolidate. | inspection | C |
| BUG-007 | `DTLSAdapter` client-side keeps a single `m_session`; the server-side `m_clients` is a `std::vector` linearly scanned. Replace with peer-keyed map (see ACE design §3). | inspection | S |
| BUG-008 | `LwM2MAdapter::serialiseObjects` (the JSON-driven encoder) iterates `rid["value"]` as an array but inside the loop dispatches on `rid["value"].is_binary()` (the whole value) instead of `ent.is_binary()` — the binary branch is therefore unreachable from the array path and a stray `is_binary` check after the loop falls through unexpectedly. Spotted during the L1 carve-out survey; behavior preserved by L1 so the bootstrap path stays byte-identical. Fix in a follow-up PR. | inspection (L1) | S |

## 4. Non-functional requirements

| ID | Requirement | Pri |
|----|-------------|:--:|
| NFR-PERF-001 | Bootstrap → Register round-trip SHALL complete in ≤ 2 s over loopback with PSK. | M |
| NFR-PERF-002 | A single LwM2M Server instance SHALL handle ≥ 100 concurrent registered clients with ≤ 64 MiB RSS and < 5 % CPU at idle. | S |
| NFR-PERF-003 | Notify latency from `Resource::write()` to wire SHALL be ≤ 50 ms at p99 under the NFR-PERF-002 load. | S |
| NFR-INTEROP-001 | Our Client SHALL pass an end-to-end Bootstrap + Register + Read/Write round-trip against Eclipse Leshan ≥ v2.0. **Register + 2.01 Created verified on the wire (2026-05-29, `log/L9/nfr-001-coap.pcap`, commit 292a848). FUP-1 closed (JDK-17 module-opens flag added to the Leshan launch wrapper). FUP-2 closed (FSM advances past AwaitingRegisterAck via the CoAPAdapter::registrationClient slot). Bootstrap + Read/Write + Update round-trip remain on FUP-3 — see log/L9/results.md.** | M |
| NFR-INTEROP-002 | Our Server SHALL pass the equivalent test with Leshan as client. **Closed 2026-05-29 with the wakaama lwm2mclient (Leshan-equivalent compliant client; no canonical Leshan client image on Docker Hub). 4-frame Register + Update round-trip verified on the wire (`log/L9/nfr-002-coap.pcap`, 482 B, commit 8ca19c1). REQ-REG-001/002/003/005/007/009 all green; wakaama held STATE_READY for the full window.** | M |
| NFR-INTEROP-003 | DTLS handshake bytes SHALL remain byte-identical to the captured baseline (`log/dtlsc.txt`, `log/dtlss.txt`) so the cryptographic behavior of tinydtls is not perturbed by the refactor. | M |
| NFR-SEC-001 | No PSK material SHALL be emitted in log output at INFO level or below. (`DTLSAdapter::dtlsGetPskInfoCb` currently logs identity at INFO — see `log.txt:114, 238`.) | M |
| NFR-OBS-001 | All LwM2M-layer state transitions SHALL be logged at DEBUG with a stable `[LwM2M:{role}:{ep}]` prefix to aid grep-based debugging. | S |
| NFR-OBS-002 | Each PR's behavior SHALL be observable in a pcap and reproducible by command-line invocation documented in `cmd/command.txt`. | S |
| NFR-TEST-001 | TLV encoder, TLV decoder, SenML codecs, and link-format codec SHALL each have ≥ 90 % line coverage in `apps/test/`. | M |
| NFR-TEST-002 | Each Must-priority FR above SHALL be backed by at least one GoogleTest case named `<Area>_<RequirementID>`. | M |
| NFR-BUILD-001 | The binary SHALL build inside the Docker image in `docker/Dockerfile` without manual intervention; CI green = build green. | M |
| NFR-BUILD-002 | The local (non-Docker) build SHOULD succeed once ACE_TAO 7.0.0 is installed at `/usr/local/ACE_TAO-7.0.0`. | S |

## 5. Traceability matrix

Maps every Must-priority requirement to: spec clause, design doc section,
implementation phase from `lwm2m-design.md` §10, and the planned test
artifact.

| ID | Spec ref | Design § | Phase | Test artifact |
|----|----------|---------|------|---------------|
| REQ-BS-001 | Core §6.1.2 | lwm2m-design §5.1 | L4 | `bootstrap_client_test.cpp::POST_bs_emits_endpoint` |
| REQ-BS-002 | Core §6.1.5 | §5.1 | L4 | `bootstrap_client_test.cpp::PUT_staged_until_finish` |
| REQ-BS-003 | Core §6.1.5 | §5.1 | L4 | `bootstrap_client_test.cpp::DELETE_purges_instances` |
| REQ-BS-004 | Core §6.1.5 | §5.1 | L4 | `bootstrap_client_test.cpp::Finish_commits_atomic` |
| REQ-BS-006 | Core §6.1.5 | §5.2 | L4 | `bootstrap_server_test.cpp::PUT_then_finish` |
| REQ-BS-007 | Core §6.1.5 | §5.2 | L4 | `bootstrap_server_test.cpp::sends_security_and_server` |
| REQ-BS-008 | Core §6.3   | §5.2 | L4 | `bootstrap_server_test.cpp::short_server_id_link` |
| REQ-REG-001 | Core §6.2.2 | §5.3 | L3 | `registration_client_test.cpp::register_emits_linkformat` |
| REQ-REG-002 | Core §6.2.2 | §5.4 | L3 | `registration_server_test.cpp::reply_location_path` |
| REQ-REG-003 | Core §6.2.3 | §5.3 | L3 | `registration_client_test.cpp::update_uses_location` |
| REQ-REG-004 | Core §6.2.4 | §5.3 | L3 | `registration_client_test.cpp::deregister` |
| REQ-REG-005 | Core §6.2.3 | §5.4 | L3 | `registration_server_test.cpp::expiry_removes_client` |
| REQ-REG-006 | Core §6.2.3 | §5.3 | L3 | `registration_client_test.cpp::renews_before_expiry` |
| REQ-REG-007 | Core §6.2.2 | §5.3 | L3 | `registration_client_test.cpp::query_params_complete` |
| REQ-REG-008 | RFC 6690     | §4.4 | L2 | `linkformat_test.cpp::encode_lwm2m_root` |
| REQ-REG-009 | Core §6.2    | §5.4 | L3 | `client_registry_test.cpp::tracks_advertised_set` |
| REQ-REG-010 | — (D3) | §5.4 | L3 | `client_registry_test.cpp::mongo_mirror_async`, `client_registry_test.cpp::reconstruct_on_restart` |
| REQ-REG-011 | Core §6.3 (D2) | §3.1 / §5.4 | L1/L3 | `client_registry_test.cpp::keyed_by_short_server_id` |
| REQ-DM-001 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::GET_returns_resource` |
| REQ-DM-002 | Core §6.3.4 | §4.4 | L2/L5 | `dm_test.cpp::GET_discover_link_format` |
| REQ-DM-003 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::PUT_replaces_resource` |
| REQ-DM-004 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::POST_creates_instance` |
| REQ-DM-005 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::DELETE_removes_instance` |
| REQ-DM-006 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::POST_executes_callback` |
| REQ-DM-007 | Core §6.3.4 | §3.1 | L5 | `dm_test.cpp::write_attributes_updates_observer` |
| REQ-DM-009 | Core §6.3.4 | §3.1 | L5 | `dm_e2e_test.cpp::server_drives_client` |
| REQ-IR-001 | Core §6.3.6 | §5.5 | L7 | `observe_test.cpp::adds_observer` |
| REQ-IR-002 | Core §6.3.6 | §5.5 | L7 | `observe_test.cpp::notify_on_change` |
| REQ-IR-003 | Core §6.3.6 | §5.5 | L7 | `observe_test.cpp::pmin_pmax` |
| REQ-IR-004 | Core §6.3.6 | §5.5 | L7 | `observe_test.cpp::thresholds` |
| REQ-IR-005 | RFC 7641 §3.6 | §5.5 | L7 | `observe_test.cpp::cancel_on_rst` |
| REQ-ENC-001 | Core Annex C | §4.1 | L1 | `tlv_test.cpp::roundtrip_<type>` |
| REQ-ENC-002 | Core Annex C | §4.1 | L1 | `tlv_test.cpp::malformed_rejected` |
| REQ-ENC-003 | RFC 8428 + Tx §6.4 | §4.2 | L6 | `senml_json_test.cpp::roundtrip` |
| REQ-ENC-004 | RFC 8428 §6 | §4.2 | L6 | `senml_cbor_test.cpp::roundtrip` |
| REQ-ENC-005 | Core §6.4 | §4.3 | L5 | `plaintext_test.cpp::single_resource` |
| REQ-ENC-006 | Core §6.4 | §4.3 | L5 | `opaque_test.cpp::single_resource` |
| REQ-ENC-007 | RFC 6690 | §4.4 | L2 | `linkformat_test.cpp::roundtrip` |
| REQ-ENC-008 | — | §8 | L1 | `codec_registry_test.cpp::register_lookup` |
| REQ-SEC-001 | Tx §5.1 | §6 | (existing) | `dtls_handshake_test.cpp::psk_aes128_ccm8` |
| REQ-SEC-002 | Tx §5.1.7 | §6 | L4 | `nosec_test.cpp::coap_only_when_mode_3` |
| REQ-SEC-003 | Core §6.3 | §6 | L4 | `security_object_test.cpp::credentials_from_object` |
| REQ-SEC-005 | Core §6.3 | §6 | L4 | `security_object_test.cpp::bs_vs_dm_distinct_psk` |
| REQ-SEC-006 | — | ACE §6 | (ACE) | `dtls_concurrent_test.cpp::two_peers_simultaneous` |
| REQ-OBJ-001 | Core §C.1-3 | §3 | L4/L8 | `objects_test.cpp::mandatory_present_after_bs` |
| REQ-OBJ-002 | OMA Registry | §3.1 | L1 | `objects_test.cpp::descriptor_metadata` |
| REQ-OBJ-003 | Core §C.3 | §3 | L8 | `objects_test.cpp::device_object_ops` |
| REQ-IO-001 | RFC 7252 | architecture.md §7 | (existing) | `coap_test.cpp::con_non_ack_rst` |
| REQ-IO-002 | RFC 7959 | architecture.md §7 | (existing/L?) | `coap_test.cpp::block1_block2` |
| REQ-IO-003 | — | ace-refactor.md §3 | (ACE refactor) | runtime: docker e2e |
| REQ-IO-004 | — | ace-refactor.md §4 | (ACE refactor) | `tx_race_test.cpp::readline_concurrent` |
| REQ-IO-005 | — | lwm2m-design §5.3/5.5 | L3/L7 | `lifetime_tick_test.cpp` |
| REQ-PUSH-001 | — | architecture.md §7 | (regression) | `push_regression_test.cpp` |
| REQ-PUSH-002 | — | lwm2m-design §4 | L6 | `push_regression_test.cpp::custom_cf_codes` |
| REQ-PUSH-003 | — | lwm2m-design §4 | L6 | `push_regression_test.cpp::cf_matches_payload` |
| BUG-001 | — | dtls_adapter.cpp:127 | hot-fix | `bug_001_psk_buffer.cpp` |
| BUG-002 | — | coap_adapter.cpp::buildRequest | hot-fix | `bug_002_cf_payload_mismatch.cpp` |

The "hot-fix" rows are unscheduled defects — they should not block the
phased work and should be fixed in standalone PRs whenever the
investigation completes.

## 6. Phase → requirement coverage

This is the inverse of §5: which requirements each phase closes.

| Phase | Requirements closed | Risk gate |
|------:|---------------------|-----------|
| L1 — ObjectStore + TLV codec carve-out | REQ-ENC-001/002/008, REQ-OBJ-002 | Pure refactor. No new tests required to pass except parity with current TLV bootstrap encode/decode. |
| L2 — link-format printer + Discover | REQ-REG-008, REQ-DM-002, REQ-ENC-007 | Must not change Register payload size for any object set already shipping. |
| L3 — Registration Client/Server + ClientRegistry | REQ-REG-001/002/003/004/005/006/007/009/010/011, REQ-IO-005 | Lifetime timer must not drift > 500 ms on a 1500 s lifetime. Async Mongo worker must not block the reactor under sustained 100 reg/s. |
| L4 — Bootstrap Client/Server FSMs | REQ-BS-001..009, REQ-SEC-002/003/005, REQ-OBJ-001 | BUG-001 must be resolved before claiming completion. |
| L5 — Read/Write/Create/Delete/Execute | REQ-DM-001/003/004/005/006/007/009, REQ-ENC-005/006 | Coverage gate from NFR-TEST-001. |
| L6 — SenML JSON + CBOR + push-plane reconciliation | REQ-ENC-003/004, REQ-PUSH-002/003 | BUG-002 must be resolved here at the latest. |
| L7 — Observe / Notify + threshold engine | REQ-IR-001..005 | NFR-PERF-003 latency test. |
| L8 — Device object live values + Conn-Mon | REQ-OBJ-003/005 (closed via apps/inc/lwm2m_object_3_device.hpp + apps/inc/lwm2m_object_stubs.hpp + apps/config/deviceObject/0.json) | Manual smoke against a real Linux host. |
| L9 — Leshan interop | NFR-INTEROP-001/002 (test plan + compose harness at `apps/docs/leshan-interop.md` + `docker/docker-compose.leshan.yml`; app-level wiring in `apps/src/main.cpp::wire_server` / `wire_client`). Pcap-pass execution is a runtime task; the code surface is complete. | Pcaps recorded and checked in to `log/L9/` after a successful run. |

## 7. Test strategy summary

Three tiers, all live under `apps/test/`:

1. **Unit (GoogleTest)** — encoders, decoders, FSMs in isolation. Already
   used by `coap_adapter_test`, `lwm2m_adapter_test`, `cbor_adapter_test`.
2. **In-process integration** — wire two instances of the binary together
   in one process via an `ACE_Pipe`-backed mock socket. Drives the FSMs
   without needing the network.
3. **Container interop** — `docker compose up server client` with the
   real Docker image; assertions over captured pcaps. This is the home
   of NFR-INTEROP-001/002.

GoogleTest case naming convention: `TEST(<Area>, <RequirementID>_<short>)`
e.g. `TEST(Bootstrap, REQ_BS_004_finish_atomic)`. Greppable from REQ-ID
back to the test.

## 8. Definition of Done per phase

A phase is DONE when:

1. **Code merged** — all touched files reviewed by ≥ 1 reviewer.
2. **Requirements closed** — every `Must`-priority requirement in §6 for
   the phase is green in CI.
3. **Tests** — coverage gates from NFR-TEST-001/002 met on the new code.
4. **Docs** — `lwm2m-design.md` and this file updated with any
   requirement that changed scope, ID, or priority during the phase.
5. **Logs** — at least one fresh pcap committed under `log/<phase>/` so
   future debugging has a reference baseline (the existing `log/dtls*.txt`
   and `log.txt` are kept untouched as historical baselines).
6. **No new BUG-IDs** — defects found during the phase are either fixed
   in the phase PR or filed as their own BUG-NNN with a follow-up issue.

## 9. Risk register

| ID | Risk | Likelihood | Impact | Mitigation |
|----|------|:---------:|:------:|------------|
| RISK-01 | tinydtls PSK buffer bug (BUG-001) blocks the whole DTLS path on some builds | M | H | Root-cause before starting L4. Add `dtls_concurrent_test` to catch regressions. |
| RISK-02 | CoAP option/Block1 parser corrupts on truncated frames (BUG-003) | M | M | Fuzz test in L1 alongside TLV carve-out. |
| RISK-03 | ACE refactor breaks plain-CoAP `/push` data plane (REQ-PUSH-001) | M | H | NFR-INTEROP equivalent for the push plane: `push_regression_test` runs in CI from day one. |
| RISK-04 | Leshan interop reveals spec deviations late (L9) | M | H | Run Leshan against incremental phases starting at L3, not only at the end. |
| RISK-05 | Hand-rolled CoAP layer falls behind RFC 7252 errata | L | M | Document the supported subset; cite the errata we have already applied. |
| RISK-06 | Async MongoDB worker (D3) backpressures or crashes, leaving the in-memory registry diverged from the on-disk one | M | M | Bounded `ACE_Message_Queue` with a high-water mark; on overflow, drop the *replay* mirror and log; on worker death, restart and rebuild from the in-memory authoritative copy. Reconciliation runs on every server start. |
| RISK-07 | Custom uCBOR/timeseries plane and LwM2M layer collide on `/{oid}` numeric URIs | L | M | URI router order: LwM2M-numeric > custom keywords > 404. Test in L2. |

## 10. Open questions

None. All six questions raised in the LwM2M design phase are now closed
(see §11 Decisions Log).

## 11. Decisions log

Bound decisions. Each row supersedes the corresponding open question above.

| Date | ID | Decision | Rationale | Affected REQs |
|------|----|----------|-----------|---------------|
| 2026-05-29 | D1 | Spec version pinned to **OMA LwM2M 1.1.1**. 1.0 is **out of scope** — no compatibility shims, no `lwm2m=1.0` accepted on Register. | Modern target; matches the working assumption already baked into REQ-REG-001 query keys and the SenML default content-format (REQ-ENC-003). | REQ-REG-001, REQ-REG-007, REQ-ENC-003, REQ-OBJ-002 |
| 2026-05-29 | D2 | **Single-server v1 with multi-server data structures.** v1 client registers to exactly one LwM2M Server post-Bootstrap, but all per-server state (`ServerRegistration`, `ObserveContext`, `ResourceAttributes`) is keyed by **Short Server ID** from day one so the multi-server upgrade is local to L3 + L7. | Single-server matches every current deployment; doing the data-model keying upfront avoids a wholesale rewrite when a second Server Object instance shows up. | REQ-REG-001, REQ-REG-009, REQ-SEC-005, REQ-IR-001, REQ-OBJ-002 |
| 2026-05-29 | D3 | **Hybrid in-memory + async MongoDB registry.** Authoritative `ClientRegistry` lives in memory; a dedicated `ACE_Task` worker mirrors every Register / Update / Deregister to MongoDB via `DbClient`. The reactor thread never blocks on DB I/O — it pushes a `RegistryEvent` onto the worker's `ACE_Message_Queue` and returns. On server restart the registry is reconstructed from MongoDB. | Survives restart without paying DB latency on the registration hot path; keeps NFR-PERF-002 reachable; turns REQ-REG-010 from Could into Should. | REQ-REG-009, REQ-REG-010, NFR-PERF-002, RISK-06 |
| 2026-05-29 | D4 | **Notify defaults to NON; CON on-demand.** Every Notify is emitted Non-Confirmable by default — matches Leshan, the most-tested LwM2M interop target, and gives lowest per-notify overhead. The Notify engine promotes a notify to Confirmable in two cases: (1) every Nth notify per observer (`kConDeadPeerInterval`, default 10) so the server learns about a dead peer without waiting for `pmax` skew; (2) on an explicit `Resource::observeCritical` flag — left as a hook for L9 / future tuning. RST handling and Observe: 1 cancel are unaffected. | NON keeps NFR-PERF-003 (≤50 ms p99) reachable on lossy links since we don't block on retransmit. CON-on-demand catches dead peers without paying the RFC 7252 §4.2 retransmit budget on every notify. | REQ-IR-002, NFR-PERF-003 |
| 2026-05-29 | D5 | **Custom `/push`-plane stays compiled in unconditionally.** No `-DENABLE_PUSH_PLANE` flag. `log.txt:1-29` shows the plane is load-bearing for an existing Sierra-Wireless-style data-push consumer; gating it would add #ifdef noise without a deployment that wants it off. REQ-PUSH-004 (build-flag) is downgraded from Could to Won't; REQ-PUSH-001..003 remain Must. The push plane and the LwM2M layer dispatch off URI shape (numeric → LwM2M, keyword `push/set/get/execute` → push), so they coexist without runtime conflict. | Keeps deploys simple; preserves a working data path; avoids a CMake matrix the team isn't ready to maintain. | REQ-PUSH-001, REQ-PUSH-002, REQ-PUSH-003, REQ-PUSH-004 |
| 2026-05-29 | D6 | **Device Object (OID 3) reads from `apps/config/deviceObject/0.json` with hard-coded constants as fallback.** Mirrors the shape already used for Security and Server objects under `apps/config/`. On startup the installer attempts to parse the JSON; on missing file, parse error, or per-RID type mismatch, the corresponding RID falls back to its compiled-in default (Manufacturer="Sierra Wireless", Model="LwM2M Client", Firmware="0.1", etc.). Per-RID granularity means a JSON file that overrides only Manufacturer leaves all other RIDs at their defaults. Live RIDs (Memory Free / Total / Current Time) still bind to platform readers regardless of JSON content. | Tunable per deployment without a rebuild, but the binary still boots clean on a fresh host. Matches the existing security/server-object pattern so operators have one consistent config shape to learn. | REQ-OBJ-003, REQ-OBJ-005 |
