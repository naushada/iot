# Leshan Interop Test Plan

L9 deliverable per design §9 and RDD NFR-INTEROP-001 / NFR-INTEROP-002.

This plan defines the **exact runtime checks** the L9 wrap-up depends on.
It is meant to be executed by a developer on a clean Docker host; the
included `docker/docker-compose.leshan.yml` brings up everything in
two containers.

## 1. Target

[Eclipse Leshan](https://github.com/eclipse-leshan/leshan), **≥ v2.0**,
running its bundled OMA test object set. Both Leshan-as-Server and
Leshan-as-Client artifacts are published as runnable jars.

## 2. Matrices

### 2.1 Our Client ↔ Leshan Server (NFR-INTEROP-001)

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start Leshan demo server | Web UI listening on :8080; CoAP/UDP on :5683; CoAPS/DTLS on :5684 |
| 2 | Start our binary: `lwm2m local=coap://0.0.0.0:56830 bs=coap://leshan:5683 role=client ep=urn:dev:client-1` | Reactor active; readline prompt visible |
| 3 | Trigger Bootstrap from the client side (Leshan demo also accepts Register without BS — we want both paths exercised) | Leshan UI lists `urn:dev:client-1`; Lifetime visible |
| 4 | From Leshan UI, Read `/3/0/0` (Manufacturer) | "Sierra Wireless" (or whatever `apps/config/deviceObject/0.json` overrides) |
| 5 | From Leshan UI, Read `/3/0` | TLV reply containing Manufacturer, Model, Serial, Firmware Version |
| 6 | From Leshan UI, Write `/3/0/15` = "Europe/Berlin" | `2.04 Changed`; subsequent Read returns "Europe/Berlin" |
| 7 | From Leshan UI, Observe `/3/0/13` (Current Time) | Initial 2.05 + Observe seq=0; periodic Notify (NON) every ~1 s |
| 8 | From Leshan UI, Execute `/3/0/4` (Reboot) | `2.04 Changed`; our binary logs the reboot hook callback |
| 9 | Stop our binary | Leshan flags the registration as expired after Lifetime + grace |

### 2.2 Leshan Client ↔ Our Server (NFR-INTEROP-002)

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start our binary: `lwm2m local=coap://0.0.0.0:5683 role=server ep=urn:dev:client-1` | Server reactor active; Bootstrap on `local`, DM on :5683 |
| 2 | Start Leshan client: `leshan-client-demo -u coap://server:5683 -n urn:dev:client-1` | Leshan logs `Registered, location=/rd/{id}` |
| 3 | From our server REPL (TBD — currently script via `dmsrv::build_*` + raw socket): Read `/3/0/0` | Leshan reply `2.05 Content` with the demo manufacturer string |
| 4 | Same against `/3/0/15` (Timezone) Write | `2.04 Changed` |
| 5 | Observe `/3/0/13` | Notify stream from Leshan client |
| 6 | Stop Leshan client cleanly (SIGINT) | DELETE `/rd/{id}` arrives; ClientRegistry size drops to 0 |

## 3. DTLS path (BUG-001 runtime regression)

Both matrices repeat with `coaps://` URIs:

```
lwm2m local=coaps://0.0.0.0:56830 bs=coaps://leshan:5684 role=client \
      identity=97554878B284CE3B727D8DD06E87659A \
      secret=3894beedaa7fe0eae6597dc350a59525 ep=urn:dev:client-1
```

Acceptance:

- tinydtls logs reach `Handshake complete` / `Peer is connected`.
- No `cannot set psk_identity -- buffer too small` warning (the symptom
  RDD BUG-001 documented in `log.txt:238-248`).
- A pcap captured during the run, when compared frame-by-frame to
  `log/dtlsc.txt` from the historical successful run, MUST agree on
  ClientHello / HelloVerifyRequest / ServerHello / ServerHelloDone /
  ClientKeyExchange / Finished. Use `tshark -r <pcap> -V -Y "dtls" \
  | diff - <(tshark -r log/dtlsc.txt -V -Y "dtls")`.

## 4. Push-plane regression (BUG-002 runtime regression)

Independent of the LwM2M flows:

1. From the client readline, issue:
   ```
   post uri="/push" uri-query="ep=A12345678ABCD" \
        data=[{"services.lwm2m.client.enable":true},{"key":"v1"}]
   ```
2. Capture the outbound packet with `tcpdump -i any -w push-regression.pcap`.
3. Open in Wireshark: the CoAP Content-Format option MUST match the
   payload bytes (12200 → CBOR head 0x82; 0 → text; 42 → opaque).
4. BUG-002 originally shipped CF=12200 with ASCII `[{"key": "v1"}]`
   (the literal seed) — this case must no longer reproduce.

## 5. NFR-PERF-002 / NFR-PERF-003 sampling

| NFR | Setup | Pass criterion |
|----|-------|----------------|
| NFR-PERF-002 | 100 simultaneous Leshan client containers registering against our server | RSS ≤ 64 MiB after stabilisation; CPU ≤ 5 % at idle (no observers active) |
| NFR-PERF-003 | One Leshan client observing `/3/0/13`; we drive `Resource::write` from a side script at 100 Hz | p99 latency from `write()` call to notify wire bytes ≤ 50 ms |

Measurement tooling is out of scope for the wiring PR — flag as a
follow-up if not yet ready.

## 6. Compose harness

`docker/docker-compose.leshan.yml` (also part of L9) wires:

- `leshan-server` — runs the official Leshan demo server.
- `iot-client` — our binary in client mode pointed at `leshan-server`.
- `leshan-client` — the official Leshan demo client pointed at
  `iot-server`.
- `iot-server` — our binary in server mode.

A single `docker compose up leshan-server iot-client` brings up the
NFR-INTEROP-001 path; `docker compose up iot-server leshan-client`
brings up NFR-INTEROP-002.

## 7. Outputs

The test plan is considered passed once:

- All steps in §2 and §3 complete with the expected outcomes.
- `tcpdump` pcaps are checked into `log/L9/` for the four runs
  (CoAP × {our-client, our-server} ∪ DTLS × {our-client, our-server}).
- BUG-001 / BUG-002 regression rows in the RDD §3.10 carry a date
  stamp confirming they didn't reappear.

## 8. Wiring follow-ups

The L9 main.cpp wiring boots the binary, drives the DTLS handshake, and
exchanges Bootstrap. Four small follow-ups extend the send loop so the
binary is actually exchanging LwM2M traffic, not just Bootstrap. Status
as of the L9 follow-up PR:

- **✅ Outbound Register POST** — `bootstrap::Client::on_done` now
  builds the Register request via `RegistrationClient::build_register_request`
  and ships it through `UDPAdapter::tx` (see
  `apps/src/main.cpp::wire_client`, "L9 stub 1").
- **✅ Outbound Update POST** — the 1 Hz client tick now builds and
  ships the Update when `RegistrationClient::should_send_update` returns
  true, then calls `note_update_sent` so the next due time is recomputed
  ("L9 stub 2").
- **✅ Observe notify dispatch** — frames produced by `DmClient::tick`
  are shipped in order via `UDPAdapter::tx` ("L9 stub 3").
- **✅ Server-side outbound DM** — periodic 30 s driver walks
  `ClientRegistry::all()` and issues `dmsrv::build_read /3/0/0` against
  each registered client through `ServiceContext_t::send_async(payload,
  peerHost, peerPort)`. A real REPL is a future follow-up; this prosaic
  poll is enough to prove the wiring ("L9 stub 4").

- **✅ FUP-2: FSM-level ACK dispatch.** `CoAPAdapter` grew a
  `registrationClient()` slot; the L9 Acknowledgement short-circuit now
  forwards the ACK to `RegistrationClient::on_response` before
  returning. With this, our client transitions
  AwaitingRegisterAck → Registered on Leshan's 2.01 reply and the
  Update tick fires at the lifetime margin. Coverage in
  `registration_client_test.cpp::FUP_2_processRequest_dispatches_ack_to_on_response`.

- **⏳ RegistryMirror** — the worker is built but not started by
  `main.cpp` because the DB schema PR is still pending. When DB writes
  land, construct `RegistryMirror(&dbClient)` in `wire_server`, attach
  via `regServer->on_event(...)` to post events, and `mirror->open()`.
  This is the only remaining wiring gap.
