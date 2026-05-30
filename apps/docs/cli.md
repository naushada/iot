# LwM2M Client CLI

The `lwm2m` binary opens an interactive REPL on the controlling
terminal whenever it is launched as `role=client` and stdin is a TTY.
Commands are dispatched through the polymorphic
[`CommandRegistry`](../inc/cli/command_registry.hpp); each command
lives in its own header/.cpp pair under
`apps/{inc,src}/cli/commands/`.

In container/CI runs stdin is typically `/dev/null` (detached mode),
so the binary skips the REPL and only runs the reactor. See
`apps/src/main.cpp::wire_client`.

---

## Quick start

The CLI is wired in both roles:

- **`role=client`** — the CLI feeds the local LwM2M client, which
  then sends the resulting CoAP frames to the configured LwM2M
  server (`bs=…`).
- **`role=server`** — the CLI feeds the local LwM2M server, which
  can issue hand-crafted requests on its own listening socket (the
  primary use is the low-level `post` for `/push`, `/set` and
  similar non-LwM2M paths).

Either way the bytes go on the wire from this binary's socket; the
target endpoint is whatever the active ServiceContext is bound to.

### As a client

```
# host
./lwm2m local=coap://0.0.0.0:56830 \
        bs=coap://leshan-iface:5683 \
        role=client \
        ep=urn:dev:client-1

LwM2MClient-->> help
LwM2MClient-->> register ep=urn:dev:client-1 lt=60
LwM2MClient-->> read     path=/3/0/0
LwM2MClient-->> quit
```

### As a server

```
./lwm2m local=coap://0.0.0.0:5683 \
        role=server \
        config=/opt/app/config

LwM2MClient-->> help
LwM2MClient-->> post uri=/push uri-query=ep=A12345 data=[{"k":"v"}] content-format=60
LwM2MClient-->> quit
```

The prompt prefix is `LwM2MClient-->> ` (set at `Readline::start()`).
Lines are history-tracked via libhistory (Up arrow recalls); tab
completes both command names and per-command argument labels.

---

## Argument grammar

Every command takes `key=value` arguments separated by whitespace.
Order does not matter; keys are case-sensitive; values may not contain
spaces unless the command's docstring explicitly says so. Values are
parsed verbatim — no quote stripping, no escape sequences.

```
write path=/3/0/15 value=Europe/Berlin       # OK
write value=Europe/Berlin path=/3/0/15       # OK (any order)
write path = /3/0/15                         # NOT OK (no spaces around =)
```

For paths, use the LwM2M convention `/<object>/<instance>[/<resource>]`.
The leading `/` is optional; the parser ignores empty path segments.

---

## Command reference

### LwM2M-level commands

These build a canonical request for the named LwM2M operation and ship
it via the client's `LwM2MClient` ServiceContext. Each one is one
CoAP frame on the wire (no Block-wise yet).

#### `bootstrap`

```
bootstrap ep=<endpoint>
```

POSTs `/bs?ep=<endpoint>` with no payload. Drives the configured
bootstrap server URI (`bs=…` CLI flag). Server replies `2.04 Changed`
on success; subsequent Bootstrap-Write / Bootstrap-Delete frames are
delivered as responses.

Example:
```
LwM2MClient-->> bootstrap ep=urn:dev:client-1
```

#### `register`

```
register ep=<endpoint> [lt=<seconds>] [b=U] [lwm2m=1.1]
```

POSTs `/rd?ep=…&lt=…&lwm2m=…&b=…` with a Content-Format 40 (link-format)
payload listing the canonical Object set:

```
</>;rt="oma.lwm2m";ct=11542,</1/0>;ver="1.1",</3/0>,</4/0>,</6/0>,</7/0>
```

Server replies `2.01 Created` with two Location-Path options
(`Location-Path: rd, <id>`) that the client uses for subsequent Update
and Deregister.

Defaults: `lt=86400`, `b=U`, `lwm2m=1.1`.

Example:
```
LwM2MClient-->> register ep=urn:dev:client-1 lt=60
```

#### `read`

```
read path=/<obj>/<inst>[/<res>]
```

GET on the path. Use Object-Instance-only paths to read all resources
in one shot (server replies in TLV / SenML), or include the resource id
to read a single value.

Examples:
```
LwM2MClient-->> read path=/3/0/0       # Manufacturer (single resource)
LwM2MClient-->> read path=/3/0         # whole Device instance
```

#### `write`

```
write path=/<obj>/<inst>/<res> value=<text>
```

PUT with the value as `text/plain` (Content-Format 0). For richer
encodings (TLV, SenML, opaque) drop to the low-level `post` command.

Example:
```
LwM2MClient-->> write path=/3/0/15 value=Europe/Berlin
```

#### `execute`

```
execute path=/<obj>/<inst>/<res> [args=<text>]
```

POST. If `args=` is supplied it is shipped as the payload with
Content-Format 0 per LwM2M §6.5.4.

Examples:
```
LwM2MClient-->> execute path=/3/0/4                       # Reboot (no args)
LwM2MClient-->> execute path=/5/0/0 args=https://srv/fw   # Firmware Update URI
```

#### `delete`

```
delete path=/<obj>/<inst>
```

DELETE on a LwM2M Object Instance (typical use: removing a registered
Server / Security / Access-Control entry).

Example:
```
LwM2MClient-->> delete path=/1/2
```

#### `observe`

```
observe path=/<obj>/<inst>[/<res>] [cancel=true]
```

GET on the path. The `cancel=true` flag is parsed but not yet wired to
the Observe option on the wire — see `apps/inc/cli/commands/observe_cmd.hpp`
note. Server-side Observe registration is currently driven by the
response routing in DmServer, not by an option from the CLI.

Examples:
```
LwM2MClient-->> observe path=/3/0/13
LwM2MClient-->> observe path=/3/0/13 cancel=true
```

---

### Data-plane commands (push plane)

These map onto the custom OMA-style push plane (project memory:
`project_lwm2m_push_plane.md` — always compiled in, no
`-DENABLE_PUSH_PLANE` flag). Each one POSTs to a dedicated URI with
`ep=<endpoint>` as the only query parameter and an optional JSON body
encoded as CBOR via `CBORAdapter::json2cbor`.

| Command | URI | Purpose |
|---------|-----|---------|
| `push`  | POST `/push?ep=…`    | Generic push toward a server |
| `set`   | POST `/set?ep=…`     | Write data point(s) |
| `get`   | POST `/get?ep=…`     | Read data point(s) |
| `exec`  | POST `/execute?ep=…` | Trigger a server-side action |

Note: the LwM2M-level commands (`read`, `write`, `execute`) operate
on object paths like `/3/0/15`. The push-plane variants (`get`, `set`,
`exec`) carry a JSON body and target the named data-plane endpoint
instead. Pick the right pair for the task — they are not interchangeable.

#### `push`

```
push ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]
```

Example:
```
LwM2MClient-->> push ep=A12345 data=[{"k":"v"}] content-format=60
LwM2MClient-->> push ep=A12345 file=/etc/payload.json
```

#### `set`

```
set ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]
```

Data-plane WRITE — companion of `get`. Body shape is whatever the
server-side handler expects (typically a JSON object/array of
key/value pairs).

Example:
```
LwM2MClient-->> set ep=A12345 data={"services.lwm2m.enable":true}
```

#### `get`

```
get ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]
```

Data-plane READ — companion of `set`. The optional payload typically
selects which keys to fetch; the response carries the values.

Example:
```
LwM2MClient-->> get ep=A12345 data=["services.lwm2m.enable"]
```

#### `exec`

```
exec ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]
```

Data-plane action trigger. Distinct from the LwM2M-level `execute`
(which operates on a resource path like `/3/0/4`).

Example:
```
LwM2MClient-->> exec ep=A12345 data={"action":"reboot"}
```

---

### Low-level CoAP escape hatch

#### `post`

```
post uri=/<path> [method=POST|GET|PUT|DELETE]
                 [uri-query=<k=v&k=v…>]
                 [data=<json>]
                 [file=<path>]
                 [content-format=<N>]
```

Hand-crafted CoAP request. Useful for non-standard URIs
(`/push`, `/set`, `/.get`, `/.execute`) or for spec-edge experimentation.

Field semantics:
- `uri=` (required) — path split on `/`
- `method=` — defaults to POST; case-insensitive
- `uri-query=` — query string split on `&`; supply each `k=v` pair
- `data=` — JSON literal, run through `CBORAdapter::json2cbor`
- `file=` — path to a file on disk; raw bytes used as payload.
  Takes precedence over `data=` when both are supplied.
- `content-format=` — numeric option value:
  - `0` text/plain
  - `40` application/link-format
  - `60` application/cbor
  - `110` application/senml+json
  - `11542` application/vnd.oma.lwm2m+tlv

Examples:
```
LwM2MClient-->> post uri=/push uri-query=ep=A12345 \
                     data=[{"k":"v"}] content-format=60

LwM2MClient-->> post uri=/.set method=PUT data={"x":1}

LwM2MClient-->> post uri=/3/0 method=GET                    # equivalent to 'read'
```

---

### Meta commands

#### `help`

```
help [<command>]
```

Without arguments, lists every command and its usage line. With a
single bare token, prints usage for that command only.

Examples:
```
LwM2MClient-->> help
LwM2MClient-->> help register
```

#### `quit`

```
quit
```

Exits the REPL. The reactor thread shuts down via `App::stop()` in
`main.cpp`. Ctrl-D (EOF) is treated as `quit`.

---

## Tab completion

The completion hook is GNU readline's standard
`rl_attempted_completion_function`, wired in `Readline::init()`
(`apps/src/readline.cpp`). Behaviour:

| Position | Completes against |
|----------|-------------------|
| Start of line (start = 0) | every registered command name |
| Anywhere after the command name | that command's `args()` labels (the `key=` strings) |

To see what a command accepts, type its name then hit `<TAB><TAB>`.

```
LwM2MClient-->> register <TAB><TAB>
b=    ep=    lt=    lwm2m=
```

---

## Adding a new command

1. Create the header in `apps/inc/cli/commands/<name>_cmd.hpp`:
   ```cpp
   #include "cli/command.hpp"
   class FooCmd : public Command {
   public:
     std::string name() const override { return "foo"; }
     std::string usage() const override { return "foo bar=<text>"; }
     std::vector<std::string> args() const override { return {"bar="}; }
     Result execute(CommandContext& ctx,
                    const std::unordered_map<std::string, std::string>& kv) override;
   };
   ```

2. Implement in `apps/src/cli/commands/<name>_cmd.cpp`. For
   LwM2M-level commands, use `cli::dispatch()` from
   [`cli/coap_dispatch.hpp`](../inc/cli/coap_dispatch.hpp) to ship the
   frame.

3. Add one line to `CommandRegistry::build_default()` in
   `apps/src/cli/command_registry.cpp`.

The CMake `GLOB_RECURSE "src/*.cpp"` picks the new .cpp up automatically;
no Makefile edit required.

---

## Non-interactive use

If the binary is launched without a TTY on stdin (`-d`, `</dev/null`,
CI runs), the REPL is skipped and the reactor runs on its own thread
until terminated. This is the path the L9 interop runners
(`log/L9/run-interop-001.sh`, `…-002.sh`) use — the bootstrap/register
FSMs in `wire_client` drive themselves on the periodic tick without
needing CLI input.

To inject CLI-style commands in a non-interactive run, either:

- Wrap the launch in `script -qc '…' /dev/null` so a PTY is allocated
  (the approach in `log/L9/run-cli-smoke.sh`); or
- Edit `wire_client` to call into `CommandRegistry::find("register")`
  directly. The same registry is reachable from the reactor thread.

---

## Related docs

- [`leshan-interop.md`](leshan-interop.md) — end-to-end wire tests
  using `register` + `read` from this CLI.
- [`lwm2m-rdd.md`](lwm2m-rdd.md) §3.7-3.8 — the LwM2M operation
  semantics each command maps to.
- [`ace-refactor.md`](ace-refactor.md) §4 — the single `UDPAdapter::tx`
  send path that `cli::dispatch` uses.
