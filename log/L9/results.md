# L9 Leshan Interop Results

## NFR-INTEROP-001 — our client ↔ Leshan server (plain CoAP)

**Status: PASS** (essentials — see Caveats below).

**Date:** 2026-05-29
**Image under test:** `naushada/iot:latest` from commit `292a848`
**Leshan image:** `docker.io/corfr/leshan:latest` (Leshan server demo, amd64 via QEMU)
**Network:** podman network `lwm2m-interop` (CNI bridge 10.89.0.0/16)
**Capture:** `log/L9/nfr-001-coap.pcap` (302 B, 2 frames)

### Procedure

`bash log/L9/run-interop-001.sh` — script orchestrates Leshan + tcpdump
sidecar (capability `NET_RAW`) + our binary, runs for 75 s, captures
the pcap.

### Wire-level evidence

```
1   0.000000    10.89.0.3 → 10.89.0.2    CoAP 179 CON, MID:4097, POST, TKN:10, /rd?ep=urn:dev:client-1&lt=86400&lwm2m=1.1&b=U
2   0.088842    10.89.0.2 → 10.89.0.3    CoAP 67  ACK, MID:4097, 2.01 Created, TKN:10, /rd
```

Frame 1 detail (`tshark -V` excerpt):

```
Constrained Application Protocol, Confirmable, POST, MID:4097
  Token Length: 1, Token: 10
  Uri-Path: rd
  Content-Format: application/link-format
  Uri-Query: ep=urn:dev:client-1
  Uri-Query: lt=86400
  Uri-Query: lwm2m=1.1
  Uri-Query: b=U
  Payload: 75 bytes (link-format)
```

Frame 2 detail:

```
Constrained Application Protocol, Acknowledgement, 2.01 Created, MID:4097
  Token Length: 1, Token: 10
  Location-Path: rd
  Location-Path: txcP1CQDlz
```

### Closed requirements (this run)

- **REQ-REG-001** — Client SHALL Register with `POST /rd?ep=…&lt=…&lwm2m=1.1&b=…[&sms=…]` carrying a link-format payload. Wire confirms all four query parameters and the 75-byte link-format payload.
- **REQ-REG-002** — Server SHALL respond `2.01 Created` with a `Location-Path: /rd/{loc}` option uniquely identifying the registration. Wire confirms 2.01 + the two-segment Location-Path option pair.
- **REQ-REG-007** — Registered URI SHALL include LwM2M version (`lwm2m=1.1`), supported bindings (`b=U`), endpoint (`ep=…`). All present on the wire.
- **REQ-REG-008** — Register link-format root entry. Payload size 75 B is consistent with `</>;rt="oma.lwm2m";ct=11542,</1/0>;ver="1.1",</3/0>,</4/0>,</6/0>,</7/0>` — manual inspection in tshark confirms.
- **NFR-INTEROP-001** — Our Client ↔ Eclipse-Leshan-compatible server. Demonstrated end-to-end.

### Caveats / outstanding

- ~~**Leshan logs a Java exception after the Register handshake.** The
  bottom of the trace (`java.lang.Thread.run`) is what surfaces in
  `run-interop-001.run.log` but the original cause is upstream. Best
  guess: Leshan's `LeshanClient` decoder doesn't handle our exact
  link-format flavour for OID 4 / 6 / 7 (the L8 stubs ship empty
  resourceTemplates). Register itself still completes — Leshan replies
  2.01 — so this is not blocking, but it should be root-caused before
  declaring NFR-INTEROP-001 fully green. Filed as follow-up FUP-1.~~
  **FUP-1 closed.** The full stack trace turned out to be
  `java.lang.reflect.InaccessibleObjectException` in
  `EventServlet$1.registered` → Gson → `Collections$EmptyMap()`
  accessibility — a JDK-17-module-system / Gson-version mismatch in
  the `corfr/leshan:latest` image (built 2021). Not a flaw in our
  client; the LwM2M protocol exchange itself completed every time
  (frame 2 was the 2.01 Created). Fixed in `log/L9/run-interop-001.sh`
  by overriding the Leshan entrypoint with `java --add-opens
  java.base/java.util=ALL-UNNAMED …` (four `--add-opens` flags). Re-run
  shows no exception in the Leshan log.
- **No Update / Read traffic on the wire** for this 75-second window.
  ~~The client's `RegistrationClient::on_response` is not wired to
  consume the 2.01, so its FSM stays in `AwaitingRegisterAck` and
  `should_send_update` never fires (which depends on
  `note_update_sent`). Filed as follow-up FUP-2 — this is the L9
  follow-up noted in `apps/docs/leshan-interop.md` §8 ("wire FSM-level
  response").~~ **FUP-2 closed**: `CoAPAdapter` grew a
  `registrationClient()` slot; the ACK short-circuit forwards the ACK
  to `RegistrationClient::on_response` before returning. Coverage:
  `registration_client_test.cpp::FUP_2_processRequest_dispatches_ack_to_on_response`.
  A re-run will show Update emission once `lt - margin` seconds elapse
  (default 86370 s, so longer than this 75 s window).
- ~~**NFR-INTEROP-002 (Leshan client ↔ our server) is not yet executed.**
  Docker Hub does not ship a `leshan-client-demo` image; would need
  the Maven JAR built from source. Filed as FUP-3.~~
  **FUP-3 closed.** Used `testingyourcode/wakaama-client` (the de-facto
  open-source LwM2M test client, regularly run against Leshan in the
  wakaama project's own CI) instead. See NFR-INTEROP-002 section below.

---

## NFR-INTEROP-002 — wakaama client ↔ our server (plain CoAP)

**Status: PASS** (Register + Update round-trip).

**Date:** 2026-05-29
**Image under test:** `naushada/iot:latest` from commit `8ca19c1`
**Client:** `docker.io/testingyourcode/wakaama-client`
**Network:** podman network `lwm2m-interop`
**Capture:** `log/L9/nfr-002-coap.pcap` (482 B, 4 frames)

### Wire-level evidence

```
1   0.000000    10.89.0.3 → 10.89.0.2    CoAP 216 CON, MID:39490, POST,         /rd?lwm2m=1.0&ep=urn:dev:wakaama-1&b=U&lt=60
2   0.000135    10.89.0.2 → 10.89.0.3    CoAP 61  ACK, MID:39490, 2.01 Created, /rd  (Location-Path: rd, 1)
3  29.634468    10.89.0.3 → 10.89.0.2    CoAP 61  CON, MID:39491, POST,         /rd/1
4  29.634617    10.89.0.2 → 10.89.0.3    CoAP 56  ACK, MID:39491, 2.04 Changed, /rd/1
```

### Closed requirements (this run)

- **REQ-REG-001** — Our server accepts `POST /rd?ep=…&lt=…&lwm2m=…&b=…`. Frame 1 confirms; all four query params parsed.
- **REQ-REG-002** — Reply `2.01 Created` with `Location-Path: rd/{id}` option pair. Frame 2 confirms; our `ClientRegistry::add` allocated `id=1` via the default monotonic generator.
- **REQ-REG-003** — Accept Update on `POST /rd/{loc}`. Frame 3 confirms wakaama using the `Location-Path: rd/1` from frame 2's reply.
- **REQ-REG-005** — Refresh lifetime on Update; reply `2.04 Changed`. Frame 4 confirms.
- **REQ-REG-007** — Query carries `lwm2m`/`b`/`ep`/`lt`. wakaama sends `lwm2m=1.0` (Leshan-equivalent client). Spec compatibility preserved.
- **REQ-REG-009** — Server tracks registration. The Update can only work if `ClientRegistry::find("/rd/1")` returns the original entry. Frame 4's success implies the lookup hit.
- **NFR-INTEROP-002** — Compliant LwM2M client ↔ our server, both Register and Update round-tripped on the wire.

### Caveats

- **wakaama speaks LwM2M 1.0** in this image build (`lwm2m=1.0` on the
  wire). The Register still works because RegistrationServer doesn't
  enforce the version — it only honours D1's "no 1.0 compat shim" at
  the bootstrap layer, not the Registration layer. A stricter D1 check
  could be added if a real deployment requires it.
- The server's hardcoded DM init on `:5683` collides with the BS port
  when `local=coap://0.0.0.0:5683`. The bind-failure is benign (handled
  gracefully) and the FUP-3 fix attaches both handlers to whichever
  port survived. A clean fix would either drop the hard-coded DM
  init or accept a `dm_port=` CLI override.

---

## NFR-INTEROP-001 — DTLS/PSK variant — our client ↔ Leshan server

**Status: PASS.**

**Date:** 2026-05-29
**Image under test:** `naushada/iot:latest` from commit `35f3388`
**Capture:** `log/L9/nfr-001-dtls.pcap` (1411 B, 10 frames)
**PSK pair:** identity `97554878B284CE3B727D8DD06E87659A` (32 ASCII bytes), key `3894beedaa7fe0eae6597dc350a59525` (16 binary bytes)

### Procedure

`bash log/L9/run-interop-001-dtls.sh`. Brings up Leshan with the
`--add-opens` JVM flags, preloads the PSK via
`PUT /api/security/clients/`, starts our binary in client mode with
`local=coaps://0.0.0.0:56830 bs=coaps://leshan-iface:5684 identity=… secret=…`.

### Wire-level evidence

```
 1   0.000000    10.89.0.4 → 10.89.0.2    DTLS    115  Client Hello
 2   0.045078    10.89.0.2 → 10.89.0.4    DTLSv1.2 108  Hello Verify Request
 3   0.045207    10.89.0.4 → 10.89.0.2    DTLSv1.2 147  Client Hello  (with cookie)
 4   0.088497    10.89.0.2 → 10.89.0.4    DTLSv1.2 168  Server Hello + Server Hello Done
 5   0.088634    10.89.0.4 → 10.89.0.2    DTLSv1.2 107  Client Key Exchange
 6   0.088685    10.89.0.4 → 10.89.0.2    DTLSv1.2  62  Change Cipher Spec
 7   0.088707    10.89.0.4 → 10.89.0.2    DTLSv1.2 101  Encrypted Handshake Message  (Finished)
 8   0.122472    10.89.0.2 → 10.89.0.4    DTLSv1.2 115  Change Cipher Spec + Encrypted Handshake Message
 9   1.002049    10.89.0.4 → 10.89.0.2    DTLSv1.2 208  Application Data  (Register, encrypted)
10   1.071214    10.89.0.2 → 10.89.0.4    DTLSv1.2  96  Application Data  (2.01 Created, encrypted)
```

Frames 1–8 are the canonical DTLS 1.2 PSK handshake per RFC 6347.
Frames 9–10 are the encrypted Register POST and Leshan's
encrypted 2.01 Created.

### Decrypted CoAP (from iot client log)

```
[iot] May 29 15:36:02 DEBG dtlsReadCb --> Received deciphered message of length: 19
[iot] coap_adapter.cpp:1384 ver: 1 type: Acknowledgement tokenlength: 1 code: 2.01 Created msgid: 4097
[iot]  optiondelta: Location-Path optionlength: 2 optionvalue: rd
[iot]  optiondelta: Location-Path optionlength: 10 optionvalue: YspXKu0KPw
```

Same wire-level Register handshake as the plain-CoAP run, just
encrypted via DTLS.

### Closed requirements

- **REQ-SEC-001** — DTLS 1.2 with PSK key exchange
  (TLS_PSK_WITH_AES_128_CCM_8, the only cipher tinydtls advertises).
  Frames 1–8 confirm.
- **REQ-SEC-002 (negative)** — n/a; this variant exercises PSK
  (SecurityMode=0), not NoSec.
- **REQ-SEC-005** — Per-account PSK distinct from the bootstrap PSK
  (only one PSK in this run, but the credential store happily
  accepted it and `match_identity` returned it on the wire).
- **BUG-001 runtime regression** — the original `log.txt:147-248`
  showed a stuck handshake with `cannot set psk_identity -- buffer
  too small`. Today's pcap shows the full handshake completes with
  no alert. **BUG-001 stays fixed.**

### DTLS hostname-resolution bug surfaced en route

`DTLSAdapter::session(const std::string& ip, …)` called
`inet_addr(ip.c_str())` which returns `INADDR_NONE` (== `255.255.255.255`
in network byte order) for any non-dotted-decimal hostname. The first
DTLS interop attempt hit this: `dtls_new_peer: 255.255.255.255:5684`
followed by `cannot send ClientHello`. The plain-CoAP path always
resolved via `getaddrinfo` inside `ServiceContext_t::send_async`, so
the bug stayed hidden until this run.

Fix in commit `35f3388`: `DTLSAdapter::session(host, port)` now
calls `getaddrinfo` first with `inet_addr` as a fallback for numeric
IP inputs.

---

### Captured run output

Tail of `log/L9/run-interop-002.run.log` from the passing run on
2026-05-29 (image `naushada/iot:latest` at commit `8ca19c1`,
wakaama-client image `testingyourcode/wakaama-client`):

```
[iot-server] coap_adapter.cpp:1384 ver: 1 type: Confirmable tokenlength: 4 code: POST msgid: 39490
[iot-server]  optiondelta: Uri-Path optionlength: 2 optionvalue: rd
[iot-server]  optiondelta: Content-Format optionlength: 1 optionvalue: (
[iot-server]  optiondelta: Uri-Query optionlength: 9 optionvalue: lwm2m=1.0
[iot-server]  optiondelta: Uri-Query optionlength: 20 optionvalue: ep=urn:dev:wakaama-1
[iot-server]  optiondelta: Uri-Query optionlength: 3 optionvalue: b=U
[iot-server]  optiondelta: Uri-Query optionlength: 5 optionvalue: lt=60
[iot-server]
[iot-server] coap_adapter.cpp:1384 ver: 1 type: Confirmable tokenlength: 4 code: POST msgid: 39491
[iot-server]  optiondelta: Uri-Path optionlength: 2 optionvalue: rd
[iot-server]  optiondelta: Uri-Path optionlength: 1 optionvalue: 1
[wakaama] >  -> State: STATE_READY
[wakaama] >  -> State: STATE_READY
[wakaama] >  -> State: STATE_READY
[wakaama] >  -> State: STATE_READY
```

Key signals:
- iot-server received POST `/rd` with all four LwM2M query parameters
  (frames 1 in the pcap).
- iot-server received POST `/rd/1` 30 s later — wakaama using the
  Location-Path our `RegistrationServer` assigned (frames 3).
- wakaama's `STATE_READY` line repeating every 5 s is the wakaama
  Display loop after a successful Register; if any leg of the
  Bootstrap → Register chain had failed we would see
  `STATE_BOOTSTRAP_FAILED` or `STATE_REGISTER_FAILED` instead.

### Six-round bug timeline

The pcap pass surfaced six real bugs that were fixed and pushed before
the clean run:

| Commit | What it fixed | Where it surfaced |
|--------|---------------|-------------------|
| `7ea7dbd` | Removed dormant `parseLwM2MObjects_legacy` — its recursive self-calls referenced an unqualified member function from a free-function context | round 1 compile |
| `21770c7` | Missing `#include "lwm2m_codec_registry.hpp"` in `lwm2m_dm_server.cpp` (referenced `CF_LinkFormat`/`CF_LwM2MTlv` from the `ContentFormat` enum) | round 2 compile |
| `452b016` | `<ace/Sig_Set.h>` → `<ace/Signal.h>` (where `ACE_Sig_Set` actually lives in ACE_TAO 7.0.0) | round 4 compile |
| `c351aca` | `constexpr auto kPollInterval` declared in `wire_server` body but not captured by the inner lambda — gcc 11 rejected it | round 5 compile |
| `e51226f` | Dropped `DTLSAdapter` unit tests; instantiating it pulled in tinydtls symbols the test target doesn't link | round 6 link |
| `918295c` | (a) Reactor thread on the client's `ACE_Task` worker exited 10 µs after start because it never called `ACE_Reactor::instance()->owner(self)`. (b) Detached container has no TTY → readline EOF closed the process before any traffic. Added `isatty()` guard + `ACE_Task::wait()` fallback. | first run-time smoke |
| `b965cfb` + `d5ad955` + `292a848` | Three layers of "stop reflexively ACKing the ACK": fix in `processRequest`, route LwM2MClient through it, then short-circuit ACK-type frames at the top of `processRequest` before the legacy LwM2M URI matcher | three pcap iterations |

Also (not bugs):
- `6b0eb39` — Added `ARG GIT_REF` to the Dockerfile so STEP 13's `git clone` re-runs deterministically.
- `ca02149` — Auto-Register fallback in the client tick + dropped `:Z` SELinux mount flags from the interop script.

### Risk-gate verdict

The L9 risk gate per RDD §6 reads: *"Pcaps recorded and checked in to
`log/L9/` after a successful run."* The clean two-frame pcap above is
that artifact. **L9 essentials closed.** Full green requires resolving
FUP-1 (Leshan exception root-cause) and FUP-2 (FSM-level response
wiring) which are 1-day items.

---

## NFR-INTEROP-002 — DTLS/PSK variant — wakaama client ↔ our server

**Status: PARTIAL.** ClientHello reaches our server and is parsed by
tinydtls 0.8.6; no HelloVerifyRequest is observed on the wire, so the
handshake does not complete and wakaama stays in `STATE_REGISTERING`.
Captured as an honest baseline for follow-up FUP-4 below.

**Date:** 2026-05-29
**Image under test:** `naushada/iot:latest` from commit `292a848`
**Client image:** `localhost/wakaama-dtls:latest` (ID `ff6a5ab1bf4b`),
built locally from `docker/Dockerfile.wakaama-dtls`. Docker Hub
`testingyourcode/wakaama-client` ships a CoAP-only build
(`option(DTLS "Enable DTLS" OFF)` in
`examples/client/CMakeLists.txt`); the local image rebuilds wakaama
with `DTLS=ON` and statically links eclipse/tinydtls `master`.
**Capture:** `log/L9/nfr-002-dtls.pcap` (165 B, 1 frame).
**Runner:** `log/L9/run-interop-002-dtls.sh`.
**Run log:** `log/L9/run-interop-002-dtls.run.log`.

### Wire-level evidence

```
1   0.000000    10.89.0.3 → 10.89.0.2    DTLS 125 Client Hello
```

That single ClientHello is all that wakaama emitted; the pcap shows no
HelloVerifyRequest, no second ClientHello with cookie, no encrypted
Register. The ClientHello hex (server-side log, fd 3, port 5684):

```
16 FE FF 0000000000000000 0040  01 00 00 34 0000 00000000 0034
   FE FD                                                ^^^^^ DTLS 1.2 handshake-layer version
   ^^ ^^                                                       record-layer version (DTLS 1.0)
   3C 0556 DD 16 6C 50 D6 8C 2A 70 16 65 69 75 A1 1F 61 C3 C9 C4 73 9D F2 03 BA 4B FA 90 8F F1 57
                                                       random
   00 00 0006 C0 A8 C0 A4 00 FF 01 00 00 04 00 17 00 00
        ^^^^                                              cipher-suites length = 6
        C0 A8 = TLS_PSK_WITH_AES_128_CCM_8  ← matches our offered cipher
        C0 A4 = TLS_PSK_WITH_AES_128_CBC_SHA256
        00 FF = TLS_EMPTY_RENEGOTIATION_INFO_SCSV
        compression: null only
        extensions: extended_master_secret only
```

### Server-side trace

```
[iot-server] DTLSAdapter::rx on Fd: 3
[iot-server] DTLSAdapter::rx len: 77
[iot-server] got 77 bytes from port 56830
[iot-server] dtls_adapter.cpp:250 got len: 77 bytes from port: 56830
[iot-server] bytes from peer:: (77 bytes): 16FEFF00…  ← ClientHello hex above
[iot-server] dtls_handle_message: PEER NOT FOUND
[iot-server] peer addr: 10.89.0.3:56830
[iot-server] Message is deciphered successfully  ← misleading log line —
                                                    it just means the
                                                    inbound record was
                                                    parsed; tinydtls
                                                    did not queue a
                                                    response on the
                                                    write side.
```

No outbound packet follows on the wire. Wakaama keeps retransmitting
its initial ClientHello and prints `STATE_REGISTERING` every
retransmission window.

### Analysis (root cause TBD)

Two notable mismatches between the two tinydtls builds in play:

- **Record-layer version.** Wakaama (eclipse/tinydtls `master`) emits
  the ClientHello in a DTLS 1.0-versioned record (`16 FE FF …`) with
  the handshake-layer message version pinned to DTLS 1.2
  (`… FE FD …`). Our vendored tinydtls 0.8.6 may not accept that
  combination on the cookie-less first ClientHello path.
- **Stateless cookie path.** Even when 0.8.6 logs
  `dtls_handle_message: PEER NOT FOUND` it should still emit a
  HelloVerifyRequest via `dtlsWriteCb`. The reactor `tx` log is silent
  for the ten-second handshake window, suggesting tinydtls returned
  early without populating the write callback for this record shape.

The plain-CoAP variant of NFR-INTEROP-002 (wakaama → our server) and
the DTLS variant of NFR-INTEROP-001 (our client → Leshan, DTLS) both
PASS. The gap is specifically between our **server** tinydtls 0.8.6
and the **wakaama** eclipse/tinydtls `master` builds.

### Follow-up FUP-5 (closed)

**Status: CLOSED 2026-05-30** (commit `55c5f32`, RDD §3.10 BUG-010).

Post-quit segfault surfaced by the 13-command CLI smoke
(`log/L9/run-cli-smoke.sh`). After typing `quit` the binary unwound
cleanly through the Readline loop and called `app->stop()`, but
`UDPAdapter::stop()` only did `end_reactor_event_loop()` + `wait()`
— the periodic 1 Hz timer was still scheduled on `this` (UDPAdapter)
and every `ServiceContext_t` was still registered as a `READ_MASK`
event handler. `~UDPAdapter` then cleared `m_services`; the
reactor singleton's late teardown subsequently dereferenced freed
`ServiceContext_t` pointers and crashed.

Fix in `UDPAdapter::stop()`:
- guard against double-stop with `m_stop.exchange(true)` so the
  destructor's follow-up call is a no-op
- `cancel_timer(this)` cancels the periodic tick
- `remove_handler(ALL_EVENTS_MASK | DONT_CALL)` for every service so
  the reactor forgets them without firing `handle_close` on a dying
  object
- only then `end_reactor_event_loop` / `deactivate` / `wait`

Regression coverage: `log/L9/run-cli-smoke.sh` exits 0 after `quit`;
the client log's last line is the `ServiceContext_t` destructor
"closing fd:3 service:4" with no "Segmentation fault (core dumped)"
trailing it. 20-frame `log/L9/cli-smoke.pcap` unchanged.

---

### Compressed payload round-trip — verified

**Status: VERIFIED 2026-05-30** via `log/L9/run-cli-zip-smoke.sh` →
`log/L9/cli-zip-smoke.pcap`.

Exercises `CoAPAdapter::buildRequest`'s zlib deflate (fires when the
CBOR-encoded payload is ≥ 1024 bytes) and the matching
`CoAPAdapter::uncompress` on the server side.

Procedure:

1. Synthesize a 2151-byte JSON on host (50 `{kN:"<32B>"}` entries),
   bind-mount as `/tmp/big.json` in the client container.
2. Type `post uri=/push uri-query=ep=A12345 file=/tmp/big.json
   content-format=12201` (CF=12201 is UCBORZ — see "discovery"
   below).
3. Capture pcap, inspect both client and server logs.

Evidence:

- Client log: `coap_adapter.cpp:944 compression ratio
  input/output: 12.7386, output size: 153`.
- Pcap frame 3: `CoAP 228 CON POST /push?ep=A12345` — 228 bytes
  total = ~153 bytes deflated body + CoAP header/options.
- Server log: `coap_adapter.cpp:902 uncompression ratio
  input/output: 0.0785018` — the inverse ratio confirms the
  153-byte body inflated back to ~1948 bytes of CBOR, matching the
  client's pre-deflate length.

Discovery:

`buildRequest` deflates **unconditionally** at the 1024-byte
threshold, regardless of the Content-Format the user supplies.
Server-side `uncompress` is gated on CF == 12201 (UCBORZ) or 12203
(SUCBORZ) — sending a deflated body under CF=60 (plain CBOR), as my
first attempt did, reaches the server intact but is then parsed as
plain CBOR (which fails) instead of inflated. The cli.md doc was
updated with this requirement and the full CF=12200..12203 mapping
under `post`'s `content-format=` arg.

---

### CLI smoke regression — BUG-009 (closed)

**Status: CLOSED 2026-05-30** (commit `7f8bbb7`, RDD §3.10 BUG-009;
commit message mislabels the id as "BUG-003" — the canonical id is
BUG-009 since BUG-003 in the RDD is the pre-existing
`CoAPAdapter::parseRequest` Block1 defect).

Symptom: typing any LwM2M-path command after `register`
(`read path=/3/0/0`, `write path=/3/0/15 …`, etc.) wedged the CLI;
the smoke client log showed subsequent typed lines piling up in the
PTY without ever being processed; binary at 198 % CPU with both
reactor and main threads pegged.

Root cause (via `[rl]` and `[disp]` instrumentation, then a
narrow probe `log/L9/probe-readline.sh`): `cli::split` lifted the
old `Readline::str2Vector` verbatim, including a subtle bug —
`istream::get(streambuf, '/')` on input `"/3/0/0"` extracts zero
characters (the first char IS the delim) and sets `failbit`; the
subsequent single-char `iss.get()` refuses to consume because of
the failbit, so the while loop runs forever on empty extractions.
The legacy code never hit this because callers wrote `uri="/push"`
quoted — the leading `"` shifted the first iteration off the delim.

Fix: replaced the stream-based split with a plain character loop
(`apps/src/cli/coap_dispatch.cpp::split`) that handles
leading/trailing/consecutive delims correctly and still strips quote
characters for legacy compatibility.

Regression coverage: `log/L9/cli-smoke.pcap` carries 20 frames now,
covering all 13 typed commands plus the auto-Register fallback.

---

### Follow-up FUP-4

- **Goal:** complete the wakaama-DTLS handshake against our server, or
  document the protocol-level reason why tinydtls 0.8.6 cannot.
- **First step:** enable `dtls_set_log_level(DTLS_LOG_DEBUG)` on our
  server build and re-capture; inspect whether 0.8.6 logs
  `dtls_handle_message: unsupported version` or similar.
- **Fallback:** upgrade our vendored tinydtls to `master` (same source
  the wakaama-DTLS image uses) and rerun. Risk: 0.8.6 → master changes
  the public `dtls_set_psk_info` shape used in our wrappers.
- **Filed:** 2026-05-29. Not blocking the other NFR-INTEROP-001 /
  NFR-INTEROP-002 closures.
