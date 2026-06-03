# L18 Plan — HTTP REST API server

> TDD-driven phase plan for the HTTP frontend to the data store.
> Each D-item closes with passing tests before the next begins.

## 0. D-items

### D1 — HttpParser (pure, no I/O)

**Scope.** `modules/http-server/src/parser.{hpp,cpp}`

Push-based HTTP/1.1 request parser. Pure logic — no sockets, no
ACE. Tests feed byte sequences and assert parse results.

```
States: MethodLine → Headers → Body → Done

MethodLine:  "GET /api/v1/db/get?timeout=30 HTTP/1.1\r\n"
Headers:     "Host: localhost\r\nContent-Length: 42\r\n\r\n"
Body:        <42 bytes>
Done:        parser.done() == true, handler fires
```

**Tests.** `parser_test.cpp` (8 tests):

| Test | What |
|------|------|
| `Parse_simple_get_no_body` | Feed `"GET / HTTP/1.1\r\n\r\n"` → method=GET, path=/, done==true |
| `Parse_post_with_body` | Feed method+headers+body → method=POST, body matches |
| `Parse_query_string` | Feed `"/api/v1/db/get?key=x&timeout=30"` → query={"key":"x","timeout":"30"} |
| `Parse_content_length_header` | Feed `"Content-Length: 42"` → body reads exactly 42 bytes |
| `Parse_incremental_feed` | Feed 1 byte at a time → parser eventually reaches Done |
| `Parse_malformed_method_line_returns_error` | Feed `"GARBAGE\r\n"` → handler not called, parser resets |
| `Parse_connection_keep_alive` | Feed `"Connection: keep-alive"` → header captured |
| `Parse_reset_for_next_request` | Parse one request, reset(), parse another → both valid |

### D2 — Router (pure, no I/O)

**Scope.** `modules/http-server/src/router.{hpp,cpp}`

Path dispatcher. Matches (method, path) → handler. Handlers are
lambdas returning `HttpResponse`.

**Tests.** `router_test.cpp` (5 tests):

| Test | What |
|------|------|
| `Route_exact_match` | Add `GET /a` → route `GET /a` returns 200 |
| `Route_method_mismatch_404` | Add `GET /a` → route `POST /a` returns 404 |
| `Route_path_mismatch_404` | Add `GET /a` → route `GET /b` returns 404 |
| `Route_multiple_routes` | Add 3 routes → each routes correctly |
| `Route_handler_receives_request` | Handler sees method, path, query, body |

### D3 — HttpSession (ACE_Svc_Handler, integration scope)

**Scope.** `modules/http-server/src/session.{hpp,cpp}`

Wires parser + router + ACE I/O. Tests use a fake ACE acceptor +
loopback connection to exercise the session without a real network.

**Tests.** `session_test.cpp` (4 tests):

| Test | What |
|------|------|
| `Session_handle_input_parse_and_respond` | Write HTTP request bytes to loopback → read response, assert 200 |
| `Session_404_on_unknown_path` | Request `GET /nonexistent` → response 404 |
| `Session_keep_alive_two_requests` | Send two requests on same connection → both get responses |
| `Session_close_on_connection_close` | Request without keep-alive → session closes after response |

### D4 — API handlers (data_store::Client integration)

**Scope.** `modules/http-server/src/handler.{hpp,cpp}`

Implements `POST /api/v1/db/get`, `POST /api/v1/db/set`,
`GET /api/v1/db/get?timeout=N`. Tests spawn a real ds-server
subprocess (same pattern as `service_gate_test.cpp`).

**Tests.** `handler_test.cpp` (6 tests):

| Test | What |
|------|------|
| `DbGet_returns_values` | Set a key, GET it via handler → value matches |
| `DbGet_missing_key_returns_null` | GET nonexistent key → value=null in JSON |
| `DbSet_writes_and_returns_changed_count` | Set 2 keys → changed=2 |
| `DbSet_schema_rejection` | Set services.ds.enable → 400 with err message |
| `LongPoll_immediate_return` | timeout=0 → returns immediately with changed=false |
| `LongPoll_waits_for_change` | Start long poll in thread, set key → poll wakes, changed=true |

### D5 — main + acceptor (wiring)

**Scope.** `modules/http-server/src/main.cpp`

Binds acceptor, creates Router with handlers, starts reactor.
Accepted via smoke test (D6).

**Tests.** No standalone unit tests — exercised by D6 smoke.

### D6 — Smoke harness + DEPLOY.md

**Scope.** `log/L18/smoke.sh`, `DEPLOY.md` update

Container-side e2e:
1. Start ds-server + iot-httpd
2. `curl -X POST -d '{"keys":["services.ds.state"]}' localhost:8080/api/v1/db/get` → 200 with state="running"
3. `curl -X POST -d '{"pairs":[{"key":"iot.endpoint","value":"test-1"}]}' localhost:8080/api/v1/db/set` → 200, changed=1
4. Start long-poll in background: `curl 'localhost:8080/api/v1/db/get?key=iot.endpoint&timeout=10'` → blocks
5. `ds-cli set iot.endpoint '"test-2"'` → long-poll curl returns with changed=true
6. `curl -X POST -d '{"keys":["services.ds.enable"]}' localhost:8080/api/v1/db/set` → 400 schema error

## 1. Acceptance

L18 closes when:
- `bash log/L18/smoke.sh` exits 0
- HttpParser passes 8 unit tests
- Router passes 5 unit tests
- HttpSession passes 4 integration tests
- API handlers pass 6 integration tests
- `DEPLOY.md` documents the HTTP API endpoints with curl examples
- All tests run under `ctest` or equivalent

## 2. File map

```
modules/http-server/
├── CMakeLists.txt                    # ACE + datastore_client + pthread
├── inc/http_server/
│   ├── parser.hpp
│   ├── session.hpp
│   ├── router.hpp
│   └── handler.hpp
├── src/
│   ├── main.cpp
│   ├── parser.cpp
│   ├── session.cpp
│   ├── router.cpp
│   └── handler.cpp
├── test/
│   ├── parser_test.cpp              # D1: 8 tests
│   ├── router_test.cpp              # D2: 5 tests
│   ├── session_test.cpp             # D3: 4 tests
│   └── handler_test.cpp             # D4: 6 tests
└── schemas/                         # (empty — uses data-store schemas)
```

## 3. Build deps

```
iot-httpd
  ├── datastore_client  (unix socket to ds-server)
  ├── ACE               (reactor, acceptor, svc_handler, task)
  ├── pthread           (ACE_Task threads)
  └── nlohmann/json     (request/response body parsing)
```
