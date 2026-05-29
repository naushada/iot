# iot — OMA LwM2M 1.1.1 Stack

A C++17 implementation of the OMA Lightweight M2M (LwM2M) device
management protocol on top of CoAP / DTLS, with the I/O layer built on
ACE (`ACE_Reactor`, `ACE_Task`, `ACE_SOCK_Dgram`).

One binary, `lwm2m`, plays either role:

| Role     | Listens on                                  | Roughly speaking            |
|----------|---------------------------------------------|------------------------------|
| `server` | `local=` Bootstrap port + `:5683` DM port   | LwM2M Server + Bootstrap-Server |
| `client` | `local=` LwM2M Client port                  | LwM2M Client (device)        |

## Status (2026-05-29)

| Phase | Closes (RDD §6) | State |
|------:|-----------------|-------|
| L1 | ObjectStore + TLV codec carve-out | ✅ |
| L2 | link-format printer + Discover | ✅ |
| L3 | Registration Client + Server + ClientRegistry + mirror skeleton | ✅ |
| L4 | Bootstrap Client + Server FSMs (BUG-001 fixed) | ✅ |
| L5 | Read / Write / Create / Delete / Execute via Resource callbacks | ✅ |
| L6 | SenML JSON + CBOR codecs (BUG-002 fixed) | ✅ |
| L7 | Observe / Notify + threshold engine (D4 NON-default / CON-on-demand) | ✅ |
| L8 | Canonical objects + Linux platform readers + JSON-backed Device | ✅ |
| L9 | App-level wiring + Leshan interop harness + first pcap pass | ✅ |

All six binding decisions (D1–D6) are recorded in
[`apps/docs/lwm2m-rdd.md`](apps/docs/lwm2m-rdd.md#11-decisions-log) and
mirrored in [`apps/docs/lwm2m-design.md`](apps/docs/lwm2m-design.md#12-decisions).

## Build

The supported build path is the container image. The local (non-container)
build needs ACE_TAO 7.0.0 at `/usr/local/ACE_TAO-7.0.0`, OpenSSL 3.1.1,
mongo-cxx-driver v3.6, tinydtls (vendored under `apps/3rdparty/tinydtls`),
and `nlohmann/json` (vendored under `apps/3rdparty/json`).

### Container (podman or docker)

```sh
podman build -t naushada/iot:latest -f docker/Dockerfile .
```

To pin a specific commit (useful for cache busting between rebuilds):

```sh
podman build --build-arg GIT_REF=<sha> -t naushada/iot:latest -f docker/Dockerfile .
```

The image:
- `ubuntu:jammy` base.
- Steps 1–7 install ACE_TAO 7.0.0 to `/usr/local/ACE_TAO-7.0.0`.
- Steps 8–9 build OpenSSL 3.1.1 from `naushada/openssl` fork.
- Steps 10–11 build `mongo-c-driver r1.19` and `mongo-cxx-driver v3.6`.
- Steps 12–13 clone googletest + the project, build tinydtls via
  autoconf, then `cmake .. && make` for the `lwm2m` + `lwm2m_test`
  targets.
- The binaries land at `/opt/app/lwm2m` and `/opt/app/lwm2m_test`.

### Local

```sh
cd apps/3rdparty/tinydtls && autoconf && autoheader && ./configure && make
cd ../.. && mkdir -p build && cd build && cmake .. && make
```

## Quick-start

Plain CoAP, server side:

```sh
./lwm2m local=coap://0.0.0.0:5783 role=server ep=urn:dev:client-1 \
        config=../config
```

Plain CoAP, client side (with auto-Register fallback for non-Bootstrap targets):

```sh
./lwm2m local=coap://0.0.0.0:56830 bs=coap://<server>:5683 role=client \
        ep=urn:dev:client-1 config=../config
```

PSK / DTLS variant — supply `identity=…` and `secret=…` (16-byte hex PSK):

```sh
./lwm2m local=coaps://0.0.0.0:5684 role=server \
        identity=97554878B284CE3B727D8DD06E87659A \
        secret=3894beedaa7fe0eae6597dc350a59525 \
        ep=urn:dev:client-1
```

CLI flags:

| Flag       | Required | Meaning                                                                  |
|------------|:--------:|--------------------------------------------------------------------------|
| `local`    | yes      | `coap[s]://host:port` to bind                                            |
| `role`     | yes      | `server` or `client`                                                     |
| `bs`       | client   | `coap[s]://host:port` of the Bootstrap or LwM2M Server                   |
| `identity` | DTLS     | PSK identity (string)                                                    |
| `secret`   | DTLS     | PSK 16-byte key in hex (32 chars)                                        |
| `ep`       | no       | Endpoint name (defaults to `urn:dev:client-1`)                           |
| `config`   | no       | Directory containing `securityObject/`, `serverObject/`, `deviceObject/` |

## Run the test binary

```sh
./apps/build/test/lwm2m_test
```

The unit-test target deliberately doesn't link tinydtls or ACE — the
~120 GoogleTest cases exercise pure-C++ logic (codecs, FSMs, registry,
observe engine, object store). Runtime regressions for DTLS / network I/O
are owned by the Leshan interop pass, not the unit-test build.

## Leshan interop pass (NFR-INTEROP-001)

Full procedure and acceptance matrices are in
[`apps/docs/leshan-interop.md`](apps/docs/leshan-interop.md). Quick path:

```sh
bash log/L9/run-interop-001.sh
```

This:
1. Brings up `corfr/leshan` server on the `lwm2m-interop` podman network.
2. Attaches a `nicolaka/netshoot` tcpdump sidecar to the Leshan netns
   (capability `NET_RAW` + `NET_ADMIN`).
3. Starts our binary in client mode pointed at Leshan.
4. Lets the Register + initial-tick window run for 75 s.
5. Tears down and leaves the capture at `log/L9/nfr-001-coap.pcap`.

The captured pcap shows our `POST /rd?ep=…&lt=86400&lwm2m=1.1&b=U` with a
75-byte link-format payload, and Leshan's `2.01 Created` reply carrying
the `Location-Path: rd/<id>` option pair — wire compliance for REQ-REG-001
and REQ-REG-002.

NFR-INTEROP-002 (Leshan client ↔ our server) is deferred — Docker Hub
does not host a Leshan client demo image, so it would need the Maven JAR
to be built from source. See `apps/docs/leshan-interop.md` §2.

## Layout

```
apps/
├── inc/           public headers (LwM2M, codecs, registration / bootstrap / observe)
├── src/           implementation files
├── test/          GoogleTest suites
├── config/        JSON config for Security (OID 0), Server (OID 1), Device (OID 3)
├── 3rdparty/      vendored tinydtls + nlohmann::json
├── docs/          design + RDD + interop docs
│   ├── architecture.md       pre- and post-L9 layout
│   ├── ace-refactor.md       I/O design
│   ├── lwm2m-design.md       module map + state machines + decisions
│   ├── lwm2m-rdd.md          requirements catalogue + traceability + defects
│   └── leshan-interop.md     L9 interop runbook
└── CMakeLists.txt

docker/
├── Dockerfile
└── docker-compose.leshan.yml four-service compose harness

log/
└── L9/            pcap captures + interop run scripts
```

## Decisions (D1–D6)

These are binding; the docs and code reference them by ID.

| ID | Decision |
|----|----------|
| D1 | Target LwM2M 1.1.1 only — no 1.0 compatibility shim, no 1.2 features. |
| D2 | v1 ships single-server, but all per-server state keyed by Short Server ID from day one. |
| D3 | Hybrid in-memory + async MongoDB mirror for the server registry. |
| D4 | Notify defaults to Non-Confirmable; every 10th notify is promoted to CON. |
| D5 | The custom `/push`/`/set`/`/get`/`/execute` data plane is always compiled in. |
| D6 | Device Object reads `apps/config/deviceObject/0.json` with per-RID fallback to compiled-in constants; live RIDs always bind to Linux readers. |

Full rationale: [`apps/docs/lwm2m-rdd.md`](apps/docs/lwm2m-rdd.md#11-decisions-log).

## Known follow-ups

- **Wire FSM-level ACK handling** — Acknowledgement-typed inbound frames
  are currently short-circuited at the top of
  `CoAPAdapter::processRequest` to suppress the wire-spurious echo. The
  proper consumer (`RegistrationClient::on_response`, `DmClient` response
  paths) needs to be wired in.
- **RegistryMirror activation** — the worker is built but not started by
  `main.cpp` because the Mongo schema PR is pending.
- **Leshan client-side interop** — needs the `leshan-client-demo` JAR
  built from source; no canonical image on Docker Hub.

See [`apps/docs/leshan-interop.md`](apps/docs/leshan-interop.md) §8 for
the full follow-up list.
