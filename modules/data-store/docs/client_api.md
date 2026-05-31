# Using `libdatastore_client` from an application

This guide is for app developers integrating against ds-server.
Reference for the wire protocol underneath every call is
[protocol.md](protocol.md); the public API header is
[`inc/data_store/client.hpp`](../inc/data_store/client.hpp).

The library is shipped as a static archive (`libdatastore_client.a`)
with an ACE-free public header — apps don't need ACE on their compile
line, only on their link line (it gets pulled in transitively).

---

## 1. CMake wiring

The data-store module lives at `modules/data-store/`. Pick one of:

### a) In-tree sibling (recommended for repos that vendor it)

```cmake
# Pull in the module — exports the datastore_client target.
add_subdirectory(${CMAKE_SOURCE_DIR}/modules/data-store
                 ${CMAKE_BINARY_DIR}/data-store)

# Headers are PUBLIC on the target; you only need it on link_libraries.
target_link_libraries(myapp PRIVATE datastore_client)
```

This is how `apps/CMakeLists.txt` does it for the `lwm2m` binary —
see commit `e43c6d9` for the canonical example.

### b) Installed (when ds-server is pre-built)

```cmake
# Adjust to where `make install` dropped the artifacts.
find_library(DATASTORE_CLIENT_LIB datastore_client
             PATHS /usr/local/lib REQUIRED)
target_include_directories(myapp PRIVATE /usr/local/include)
target_link_libraries(myapp PRIVATE ${DATASTORE_CLIENT_LIB} ACE pthread)
```

ACE + pthread come back as transitive link deps; on Linux you also
need `dl` for the bundled Lua statics if you re-bundle ds-server too,
but pure clients don't.

---

## 2. The 30-second example

```cpp
#include <iostream>
#include <data_store/client.hpp>

int main() {
    data_store::Client cli;

    // 1. Open the connection. Empty path = the compile-time default
    //    (/var/run/iot/data_store.sock). Pass a custom path for tests.
    if (auto s = cli.connect(); !s.ok) {
        std::cerr << "connect: " << s.err << "\n";
        return 1;
    }

    // 2. Write a typed value.
    cli.set("iot.endpoint", std::string("urn:dev:client-1"));
    cli.set("iot.lifetime", static_cast<std::uint32_t>(86400));
    cli.set("iot.enabled",  true);

    // 3. Read back.
    std::vector<data_store::Client::GetResult> got;
    cli.get({"iot.endpoint", "iot.lifetime", "iot.nope"}, got);
    for (const auto& r : got) {
        std::cout << r.key << " present? " << r.has_value << "\n";
    }
    // cli destructor calls close() automatically.
}
```

That's the entire happy path. Every call returns `Status{ok, code, err}`
— check `ok` for binary success/failure, read `err` for a one-line
diagnostic, and inspect `code` for the wire-level status (see
[protocol.md §5](protocol.md#5-status-codes)) when you need to branch
on the specific failure mode.

---

## 3. Connecting and lifecycle

```cpp
data_store::Client cli;
auto s = cli.connect("/run/myapp/ds.sock");      // path optional
if (!s.ok) { /* server not up, socket missing, perms denied, … */ }
```

Behaviour:

- **Non-blocking ish.** `connect()` does the TCP-style handshake with a
  5 s wall-clock timeout. On success a background **listener thread**
  is spawned to demux responses + pushes from the socket.
- **No welcome handshake** — EMP is wire-ready immediately on connect.
- **Idempotent close.** `close()` (and the destructor) joins the
  listener and tears down the socket. Calling `close()` twice is fine.

```cpp
// Explicit teardown — fine to call from any thread.
cli.close();
```

The library is **move-constructible** but **not copyable**:

```cpp
data_store::Client cli;
cli.connect();
auto moved = std::move(cli);    // ok — the socket + listener move with it.
```

A failed connect is a normal application condition; do not treat it as
fatal unless your app genuinely cannot proceed without the data
store. In the iot binary, ds-server is **optional config plane** — see
`apps/src/ds_config.cpp` for the "log + fall back to defaults" pattern.

---

## 4. Typed values

Every value on the wire is a `data_store::Value`, defined as:

```cpp
using Value = std::variant<
    std::monostate,      // JSON null
    std::string,
    bool,
    std::uint32_t,
    std::int32_t,
    double>;
```

This is intentionally the same shape as grace-server's `value_type`.
Construct it from any of the alternatives:

```cpp
using V = data_store::Value;

V a = std::string("text");       // string
V b = true;                      // bool
V c = std::uint32_t{42};         // uint
V d = std::int32_t{-7};          // int (signed)
V e = 1.5;                       // double
V f = std::monostate{};          // null

cli.set("a", a);
cli.set("b", b);
cli.set("c", c);
```

Two gotchas worth knowing:

- **Bare `42` is ambiguous** until you cast — `int` doesn't fit the
  variant directly. Use `std::uint32_t{42}` (or wrap in `V{42u}`).
- **Bare string literals** also need an explicit cast because
  `const char*` ambiguously converts to both `std::string` and `bool`:
  use `std::string("hello")` or `V{std::string("hello")}`.

Read back via `std::get`/`std::visit`:

```cpp
std::vector<data_store::Client::GetResult> got;
cli.get({"a", "b", "c"}, got);

for (const auto& r : got) {
    if (!r.has_value) { /* key not in the store */ continue; }
    std::visit([&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>)
            std::cout << r.key << " (string) = " << v << "\n";
        else if constexpr (std::is_same_v<T, bool>)
            std::cout << r.key << " (bool) = " << v << "\n";
        else if constexpr (std::is_same_v<T, std::uint32_t> ||
                           std::is_same_v<T, std::int32_t> ||
                           std::is_same_v<T, double>)
            std::cout << r.key << " (numeric) = " << v << "\n";
        else
            std::cout << r.key << " (null)\n";
    }, r.value);
}
```

For the common "I just want this key as type T or a default" idiom:

```cpp
template <class T>
T value_or(data_store::Client& cli, const std::string& k, T fallback) {
    std::vector<data_store::Client::GetResult> got;
    if (!cli.get({k}, got).ok || got.empty() || !got[0].has_value)
        return fallback;
    if (auto* v = std::get_if<T>(&got[0].value)) return *v;
    return fallback;
}

auto lifetime = value_or<std::uint32_t>(cli, "iot.lifetime", 86400);
```

---

## 5. `set` — single, multi, atomic

```cpp
// Single key, convenience overload.
cli.set("counter", std::uint32_t{1});

// Multiple keys atomically — server processes the whole batch under
// one mutex hold, so peers see all-or-nothing for the batch.
cli.set({{"app.host", std::string("alpha")},
         {"app.port", std::uint32_t{5683}},
         {"app.tls",  true}});
```

`set` is idempotent in a useful way: writing the same value that
already exists **does not** fire a notify push to watchers
(`REQ-DS-006`). Build idempotent producers — there's no need to
read-then-set just to avoid notify storms.

The returned `Status` is `ok=true` on success, `ok=false` with `code`
set to the wire status on failure. Common failure codes:

| `code`            | Meaning                                                   |
|-------------------|-----------------------------------------------------------|
| 0                 | success                                                   |
| `0x8003 BadPayload` | sent an entry that wasn't a single-key object             |
| `0x8004 SchemaRejected` | schema (`SchemaRegistry`) rejected the value type/range |

---

## 6. `get` — many keys, partial-ok

```cpp
std::vector<data_store::Client::GetResult> got;
auto s = cli.get({"present", "missing"}, got);
if (!s.ok) { /* whole request failed (timeout, bad payload, …) */ }

// got[i].key matches keys[i]; got[i].has_value=false when the key is
// neither in the store NOR backed by a schema default.
for (const auto& r : got) { /* … */ }
```

Schema interaction: if ds-server was started with `ds-schema-dir=…`
and the schema declares a `default` for the key, an absent key comes
back with `has_value=true` and `value` filled from the schema default.
Distinguish "absent + no default" (caller's `has_value=false`) from
"absent but default applied" (`has_value=true, value=<default>`).

---

## 7. Watch + notify — two complementary styles

The library exposes the same wire subscription through two surfaces.
You can mix both on the same `Client`; the listener thread fans every
matching event to both paths.

### Callback-style (recommended for most apps)

```cpp
data_store::Client::WatchHandle handle = data_store::Client::kInvalidHandle;

auto s = cli.watch(
    {"iot.endpoint", "iot.lifetime"},
    [](const data_store::Client::Event& ev) {
        // Fires on the library's internal listener thread.
        // DO NOT block here — push real work to your own queue/reactor.
        std::cout << "[ev] " << ev.key
                  << " has_prev=" << ev.prev_has_value << "\n";
    },
    &handle);

// ... later, to stop receiving:
cli.unwatch(handle);
```

Three things to know:

1. **The callback runs on the listener thread.** Don't do long-running
   work or take locks that your main thread might also try to take
   without expecting reentrancy.
2. **Local refcounting is automatic.** Calling `watch(["foo"], ...)`
   N times only emits *one* `RegisterWatch` on the wire; the Nth
   `unwatch` drops the subscription server-side. Multiple local
   callbacks on the same key are independent.
3. **One handle per `watch()` call.** Keep them around if you want
   to drop subscriptions individually; otherwise pass `nullptr` for
   `out_handle` and let `close()` clean up at shutdown.

### Pull-style (for event-loop integrations)

```cpp
cli.watch({"iot.endpoint"});             // no callback — pull only

while (running) {
    data_store::Client::Event ev;
    if (auto s = cli.recv_event(ev, /*timeout_ms=*/1000); s.ok) {
        // Runs on YOUR calling thread — safe to do anything here.
        handle(ev);
    } else if (s.code == ETIMEDOUT) {
        continue;     // just a quiet tick
    } else {
        break;        // connection closed
    }
}
```

Good for integrating into an existing select/poll/reactor loop without
spawning extra threads.

### Both at once

Calling callback-style `watch()` + pull-style `watch()` on the same
keys is fine — every event lands on both paths. Useful when you want
the callback to update an in-memory cache and the pull-side to feed a
metrics emitter, for example.

---

## 8. Threading rules

| Resource                | Thread           | Notes                                                  |
|-------------------------|------------------|--------------------------------------------------------|
| `Client::connect/close` | any              | `close()` joins the listener; don't call from inside the callback |
| `set`/`get`             | any              | Concurrent calls from N threads serialise on the socket send + per-request promise |
| `watch`/`unwatch`       | any              | Thread-safe; refcount is held under an internal mutex  |
| `recv_event`            | any              | Blocks on a condvar; multiple consumers fan competitively |
| `EventCallback` body    | **listener**     | Do NOT block; do NOT call `cli.close()` from inside    |

The library guarantees one outstanding response per `(opcode, reqID)`
pair. Internally it combines them into a 16-bit correlator so two
opcodes can share a reqID slot in flight; you don't need to worry
about per-call ID assignment.

---

## 9. Error handling patterns

```cpp
auto s = cli.set("k", std::string("v"));
if (!s.ok) {
    // Three signals worth looking at:
    switch (s.code) {
        case 0:
            // ok was false but code is 0 → library-level error
            // (connection dropped, request timed out, etc.)
            // — s.err is the diagnostic.
            break;
        case ETIMEDOUT:
            // Server didn't respond within timeout_ms.
            break;
        case static_cast<int>(data_store::proto::Status::SchemaRejected):
            // Schema validation failed; s.err carries the per-key reason.
            break;
        default:
            // Other 0x8xxx Status code from the server.
            break;
    }
}
```

For a guarantee that you get the latest known value even if ds-server
is briefly unreachable:

```cpp
template <class T>
T cached_or_fetch(data_store::Client& cli,
                  const std::string& k,
                  std::optional<T>& cache,
                  T fallback) {
    std::vector<data_store::Client::GetResult> got;
    if (cli.get({k}, got).ok && !got.empty() && got[0].has_value) {
        if (auto* v = std::get_if<T>(&got[0].value)) {
            cache = *v;
            return *v;
        }
    }
    return cache.value_or(fallback);
}
```

Useful when ds-server restarts mid-run but you want last-known config
rather than the compiled default.

---

## 10. Putting it together — a realistic startup

The pattern the iot binary uses (`apps/src/ds_config.cpp` + `main.cpp`):

```cpp
#include <data_store/client.hpp>
#include <iostream>

struct AppConfig {
    std::string  endpoint  = "urn:dev:client-fallback";
    std::uint32_t lifetime = 86400;
    std::string  server_uri = "coap://localhost:5683";
};

AppConfig load_config(const std::string& ds_sock) {
    AppConfig c;
    data_store::Client cli;
    if (!cli.connect(ds_sock).ok) {
        std::cout << "[app] ds-server unavailable; using built-in defaults\n";
        return c;
    }

    std::vector<data_store::Client::GetResult> got;
    cli.get({"iot.endpoint", "iot.lifetime", "iot.server.uri"}, got);
    for (const auto& r : got) {
        if (!r.has_value) continue;
        if (r.key == "iot.endpoint") {
            if (auto* v = std::get_if<std::string>(&r.value)) c.endpoint = *v;
        } else if (r.key == "iot.lifetime") {
            if (auto* v = std::get_if<std::uint32_t>(&r.value)) c.lifetime = *v;
        } else if (r.key == "iot.server.uri") {
            if (auto* v = std::get_if<std::string>(&r.value)) c.server_uri = *v;
        }
    }
    // cli.close() happens at scope exit.
    return c;
}
```

For long-lived processes that want to react to config changes without
restart, keep a single `Client` alive for the process lifetime and
register a callback-style watch for the keys you care about — the
callback updates an `std::atomic` / locks a `std::mutex`-protected
config struct and signals the rest of the app to re-read.

---

## 11. Testing tips

- **Run ds-server out-of-process in your integration tests** — spawn
  it as a subprocess pointing at a per-test `mkdtemp` socket. That's
  what `modules/data-store/test/protocol_test.cpp` does.
- **Inject a fake socket path** via your app's config so unit tests
  don't depend on the production default. The iot binary does this
  with `ds-sock=` on the CLI.
- **Don't share a `Client` between forked processes.** The listener
  thread is per-process; fork the connection too if you fork the app.
- **For deterministic notify ordering**, set the value on a separate
  `Client` from the one watching — the server emits the response
  before fanning out pushes, so the originating client sees ack first
  while peers see the push.

---

## 12. Where to look next

- [protocol.md](protocol.md) — the wire-level EMP framing this library
  speaks.
- [`inc/data_store/client.hpp`](../inc/data_store/client.hpp) — the
  authoritative API reference (the comments are doxygen-style).
- [`apps/src/ds_config.cpp`](../../../apps/src/ds_config.cpp) — a
  worked example of optional, best-effort use from a real binary.
- [tdd.md](tdd.md) — internal design + ownership rules if you're
  extending the module rather than just using it.
