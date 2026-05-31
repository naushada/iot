# Data-Store Wire Protocol (EMP)

> Modelled on Eclipse Mihini's
> [Embedded Micro Protocol](https://wiki.eclipse.org/Mihini/Embedded_Micro_Protocol).
> Same framing shape; smaller, KV-shaped opcode set tailored to this
> module's contract.

The data-store speaks a binary, framed RPC protocol over `AF_UNIX`
SOCK_STREAM (default socket `/var/run/iot/data_store.sock`). Every
exchange is one or more EMP frames; payloads carry UTF-8 JSON.

The framing is symmetric — both client → server commands and
server → client responses + pushes use the same 8-byte header.

---

## 1. Frame layout

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             cmdID             |      type     |     reqID     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         payload_size                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                  payload  (payload_size bytes)                |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

All integers are **big-endian**.

| Field          | Size  | Description                                       |
|----------------|------:|---------------------------------------------------|
| `cmdID`        | 2 B   | opcode (see §2)                                   |
| `type`         | 1 B   | frame-kind bits (see §3)                          |
| `reqID`        | 1 B   | request correlator, 0..255                        |
| `payload_size` | 4 B   | bytes of payload that follow (≤ 1 MiB ceiling)    |
| `payload`      | N B   | request/response/push body (see §4)               |

The implementation header layout: `modules/data-store/inc/data_store/proto.hpp`.

---

## 2. Opcodes

| cmdID    | Name             | Direction        | Purpose                                       |
|---------:|------------------|------------------|-----------------------------------------------|
| `0x0001` | `Set`            | client → server  | Write one or more keys (atomic per request)   |
| `0x0002` | `Get`            | client → server  | Read one or more keys                         |
| `0x0003` | `RegisterWatch`  | client → server  | Subscribe to value-change notifications       |
| `0x0004` | `RemoveWatch`    | client → server  | Unsubscribe one or more watched keys          |
| `0x0064` | `NotifyEvent`    | server → client  | Push: value of a watched key changed          |

Any opcode the server doesn't recognise is answered with a response
carrying status `BadOpcode` (§5).

The full Mihini opcode space (52 commands covering SMS / FOTA / data
consolidation) is **not** implemented — those concepts don't map onto a
generic KV store. The five opcodes above are the data-store's complete
public surface.

---

## 3. Type byte

The `type` byte tells the receiver what kind of frame it is.

| bit | Mnemonic   | Set when…                                                 |
|----:|------------|-----------------------------------------------------------|
| 0   | `Response` | server → client reply, correlated by `reqID`              |
| 1   | `Push`     | server → client unsolicited push (`reqID = 0`)            |
| 2-7 | reserved   | MUST be zero                                              |

Encoding rules:
- **Command** (client → server): `type = 0x00`, `reqID > 0`.
- **Response** (server → client): `type = 0x01`, `reqID` echoed from
  the originating command.
- **Push** (server → client): `type = 0x02`, `reqID = 0`.

Mihini's EMP reserves bits 1-7; we repurpose **bit 1** for server-
initiated push so that change-notifications fit the same framing as
everything else. This is a local extension — Mihini-pure peers would
treat such a frame as malformed, but no Mihini-pure peer ever connects
to ds-server.

---

## 4. Payload

### Commands

The command body is the JSON object (or empty for opcodes that take no
arguments). Every current opcode carries a single field `keys`:

```json
// Op::Set        — array of single-key objects, value is JSON-typed.
{"keys":[{"foo":"bar"}, {"counter":42}, {"enabled":true}]}

// Op::Get        — array of strings.
{"keys":["foo","missing"]}

// Op::RegisterWatch / Op::RemoveWatch  — array of strings.
{"keys":["foo","bar"]}
```

Value types round-trip through `data_store::Value`:

| JSON in           | Stored as                         |
|-------------------|-----------------------------------|
| `null`            | `std::monostate`                  |
| `true` / `false`  | `bool`                            |
| `"text"`          | `std::string`                     |
| `42` (≥ 0, ≤ 2³²) | `std::uint32_t`                   |
| `-1`              | `std::int32_t`                    |
| `1.5`             | `double`                          |
| out-of-range int  | spilled to `double`               |

### Responses

The payload's first 2 bytes are the big-endian `Status` code (§5);
the rest, if any, is a JSON body.

```text
+--------+---------------+
| status |   JSON body   |
| 2 B BE |   (optional)  |
+--------+---------------+
```

| Op              | Body on success                                    | Body on failure                   |
|-----------------|----------------------------------------------------|-----------------------------------|
| `Set`           | empty                                              | `{"err":"..."}`                   |
| `Get`           | `{"data":[{"k":"…","v":<typed-or-null>}, …]}`      | `{"err":"..."}`                   |
| `RegisterWatch` | empty                                              | `{"err":"..."}`                   |
| `RemoveWatch`   | empty                                              | `{"err":"..."}`                   |

For `Get`, every requested key produces one entry in `data` in the
same order; `v == null` means the key is absent from the store **and**
the (optional) schema has no default for it.

### Pushes

`NotifyEvent` (only push opcode today) carries no status prefix —
the payload IS the JSON body:

```json
{"k":"foo","v":"bar","prev":"old"}
```

`prev` is `null` when the key was newly created. `v` is `null` when
the change was a removal (currently no opcode emits this — `Set` only
fires for actual writes — but the field is reserved).

---

## 5. Status codes

`Status` is the 2-byte big-endian integer that prefixes every response
payload. **Zero is success**, every other value is a failure.

| Code     | Name             | Meaning                                                                   |
|---------:|------------------|---------------------------------------------------------------------------|
| `0x0000` | `Ok`             | Success                                                                   |
| `0x8001` | `BadFrame`       | Malformed header, payload truncated, oversized, or wrong direction        |
| `0x8002` | `BadOpcode`      | Unknown / unsupported `cmdID`                                             |
| `0x8003` | `BadPayload`     | JSON parse failure or payload not in the expected shape for this opcode   |
| `0x8004` | `SchemaRejected` | `SchemaRegistry::validate_set` rejected the value                         |
| `0x8005` | `NotFound`       | Reserved for future single-key reads                                      |
| `0x8006` | `InternalError`  | Caught exception on the server side                                       |

The high bit distinguishes implementation status codes (`0x8xxx`) from
the reserved low range (`0x0000-0x7FFF`) used by transport / OS errors
in Mihini's original `swi_status_t` mapping.

---

## 6. Request lifecycle

1. **Connect.** Client opens the unix socket. There is no welcome
   handshake — the connection is usable immediately.
2. **Issue commands.** Client picks the next `reqID` (round-robin in
   1..255; the lib's correlator combines `(op << 8) | reqID` so two
   different opcodes can share a `reqID` slot in flight).
3. **Receive responses.** Server replies on the same socket with a
   frame whose `type` has bit 0 set and `reqID` matches the request.
   Order between commands on different reqIDs is **not** guaranteed.
4. **Receive pushes.** Independently of any request, the server may
   push `NotifyEvent` frames for keys this session has registered.
   Pushes have `reqID = 0` and don't correlate with any request.

The per-session `reqID` namespace is 8-bit, so a client must wait for
the response before reusing the same `(op, reqID)` pair. The client
library handles this transparently with a monotonic counter that wraps.

---

## 7. Worked example — `Set foo=42`

Wire bytes the client sends (28 B total):

```
00 01    ← cmdID = Set
00       ← type  = command
07       ← reqID = 7
00 00 00 14 ← payload_size = 20
{"keys":[{"foo":42}]}    ← 20 B JSON body
```

Wire bytes the server sends back (10 B total):

```
00 01    ← cmdID = Set (echo)
01       ← type  = response (bit 0)
07       ← reqID = 7 (echo)
00 00 00 02 ← payload_size = 2 (status prefix only, empty body)
00 00    ← Status::Ok
```

If a peer is currently watching `foo`, the server also emits a push to
that peer in parallel (28 B):

```
00 64    ← cmdID = NotifyEvent
02       ← type  = push (bit 1)
00       ← reqID = 0
00 00 00 14 ← payload_size = 20
{"k":"foo","v":42,"prev":null}  ← 20 B JSON body
```

---

## 8. Client API surface

`libdatastore_client` exposes one method per opcode plus convenience
wrappers. Per [feedback_ace_logging] the server logs go through ACE;
clients keep the call surface POSIX-clean.

| Method                                                | Opcode            |
|-------------------------------------------------------|-------------------|
| `Client::connect(path)`                               | (transport)       |
| `Client::set(vector<KV>, timeout)`                    | `0x0001 Set`      |
| `Client::set(key, Value, timeout)` (convenience)      | `0x0001 Set`      |
| `Client::get(vector<string>, vector<GetResult>&, …)`  | `0x0002 Get`      |
| `Client::watch(vector<string>, cb, *handle, …)`       | `0x0003 Register` |
| `Client::watch(vector<string>, …)` (pull-style)       | `0x0003 Register` |
| `Client::unwatch(WatchHandle, …)`                     | `0x0004 Remove`   |
| `Client::unwatch(vector<string>, …)`                  | `0x0004 Remove`   |
| `Client::recv_event(Event&, …)`                       | (consumes push)   |
| `EventCallback` invoked from listener thread          | `0x0064 Notify`   |

Each call serialises the appropriate command frame, waits up to
`timeout_ms` for the matching response, and lifts the wire `Status`
into the returned `Status{ok, code, err}`.

---

## 9. Sanity ceilings and failure modes

- **Maximum payload**: 1 MiB. A header claiming more is treated as
  corrupt and the connection is dropped (`BadFrame`).
- **Truncated recv**: client and server both buffer partial frames and
  retry on next read — never block.
- **Cross-opcode reqID collisions**: avoided client-side by combining
  `(op, reqID)` into a 16-bit correlator key.
- **Listener thread crash**: client's outstanding promises are failed
  with `runtime_error("listener exited")`; future requests get
  `Status::ok = false` with `code = ECONNRESET`-equivalent diagnostic.

---

## 10. Divergences from Mihini EMP

| Aspect              | Mihini EMP                              | Data-store EMP                                |
|---------------------|-----------------------------------------|-----------------------------------------------|
| Transport           | TCP `localhost:9999`                    | `AF_UNIX` SOCK_STREAM, default `/var/run/iot/data_store.sock` |
| Opcode set          | 52 commands (SMS, FOTA, tables, …)      | 5 commands (`Set`/`Get`/`Reg`/`Rem`/`Notify`) |
| Push mechanism      | Implicit per command-pair (e.g. `NewSMS`) | Explicit type-byte bit 1                    |
| Status codes        | `swi_status_t`                           | Custom 0x8xxx range                          |
| Welcome             | n/a                                      | n/a                                          |
| Payload type        | JSON                                    | JSON, with typed values via `data_store::Value` |
| reqID width         | 8 bits                                   | 8 bits (matched)                            |
| Endian              | Big-endian                              | Big-endian (matched)                         |
