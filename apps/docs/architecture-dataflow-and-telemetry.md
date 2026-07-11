# Data-flow architecture: data-store, UI updates, vehicle telemetry & map

How a daemon publishes/subscribes a data-point, how a change reaches the UI, how
vehicle telemetry is stored, and how a vehicle is tracked on the map — with the
real component, method, and message names from the code.

> **Live vs designed.** The telemetry map + 60-day history ride **`lwm2m-dm`
> server-Reads** of LwM2M Objects 6 & 33000 today. The LwM2M **Send/SenML** path
> is coded + unit-tested but its session-I/O glue and server persist are **gated
> off / stubbed** — it is *not* on the live path. Divergences are called out.

---

## 0. Components at a glance

```mermaid
flowchart LR
  subgraph device["Device (RPi / mangOH)"]
    CAN["ECU / OBD-II bus"] -->|"SocketCAN 0x7DF"| VD["iot-vehicled"]
    GNSS["WP modem GNSS"] --> CC["cellular-client"]
    VD -->|"set_volatile vehicle.*"| DS[("ds-server<br/>/var/run/iot/data_store.sock")]
    CC -->|"set gps.* / cell.*"| DS
    LC["lwm2m client"] -->|"get vehicle.*/gps.*"| DS
    HTTPD_D["iot-httpd (device)"] -->|"watch + get"| DS
    UI_D["device-ui (Angular)"] -->|"GET /api/v1/status?timeout"| HTTPD_D
  end
  subgraph cloud["Cloud"]
    DM["lwm2m-dm server"] -->|"set_volatile cloud.vehicle.telemetry"| CDS[("cloud ds-server")]
    HTTPD_C["iot-httpd (cloud)"] -->|"watch + get"| CDS
    HTTPD_C -->|"spool NDJSON"| SPOOL["/var/lib/iot/telemetry-spool"]
    ING["iot-telemetry-ingest (sidecar)"] -->|"mongoimport"| MONGO[("Mongo db=iot<br/>coll=telemetry")]
    MAP["cloud-ui map (Leaflet)"] -->|"POST /api/v1/db/get"| HTTPD_C
  end
  LC <==>|"DTLS :5683<br/>CoAP Read Obj 6 / 33000"| DM
```

---

## 1. App ⇄ data-store: publish & subscribe a data-point

The data-store (**ds**) is a typed key/value plane. Daemons talk to `ds-server`
over a unix socket (`/var/run/iot/data_store.sock`) using the **EMP** wire
protocol (`modules/data-store/inc/data_store/proto.hpp`): an 8-byte big-endian
header `{cmdID, type, reqID, size}` + JSON body.

Ops (`proto::Op`): `Set 0x0001`, `Get 0x0002`, `RegisterWatch 0x0003`,
`RemoveWatch 0x0004`, `NotifyEvent 0x0064` (server→client **push**).

Keys are typed by `*.lua` **schema** files loaded from `/etc/iot/ds-schemas/`
(`type`, `access`, `default`, `min`/`max`, `read_acl`/`write_acl`). A `Set` is
validated against the schema (`Status::SchemaRejected` on mismatch). **Persistent**
keys write `m_data` and flush to `/var/lib/iot/data_store.lua`; **volatile** keys
(`set_volatile`) write an in-RAM overlay only — used for fast-changing telemetry —
and are lost on server restart. Both emit identical `NotifyEvent` pushes.

```mermaid
sequenceDiagram
  autonumber
  participant App as Daemon (data_store::Client)
  participant DS as ds-server (Worker + DataStore)
  participant W as Another watcher (e.g. iot-httpd)

  Note over App,DS: connect() — ACE unix SOCK_STREAM, spawns listener thread
  App->>DS: Op::Get {"keys":["cell.apn",...]}
  DS-->>App: Response(Ok) {values}  %% unset key → schema default_for(k)

  Note over App,DS: subscribe a data-point
  App->>DS: Op::RegisterWatch {"keys":["sms.send.request"]}
  DS->>DS: check_read_acl, m_watchers[key] += Session*
  DS-->>App: Response(Ok)  → WatchHandle

  Note over App,DS: publish a data-point
  App->>DS: Op::Set {"keys":[{"cell.state":"registered"},{"cell.version":"7"}]}
  DS->>DS: validate_set (schema), DataStore::set → SetResult{prev,changed,watchers}
  DS-->>App: Response(Ok)  %% ack to writer first
  alt value actually changed (REQ-DS-006)
    DS-->>W: NotifyEvent push {k:"cell.version", v:"7", prev:"6"}
  else unchanged
    DS--xW: (no notify, no flush)
  end
```

**Concrete example — `cellular-client`** (`modules/wan/cellular/daemon/cellular_client.cpp`):

1. `m_ds.connect(...)` → 2. `load_config_from_ds()` issues `get` for
`cell.apn`/`cell.modem.tty`/… (Admin read keys) → 3.
`watch("sms.send.request", on_send_request, &wh)` → 4. every poll
`publish()` builds `m_state.to_kv()` and `set(...)` the Viewer status keys
(`cell.state/operator/signal.dbm/ip/…`) plus the **bump counter `cell.version`**.
`sms.send.status` progress uses `set_volatile`.

> The **`*.version` bump keys** are the trick that makes the UI cheap: the daemon
> increments `cell.version` only when something actually changed, and that single
> key is what the `/status` long-poll watches.

---

## 2. How an update reaches the UI (long-poll round trip)

`iot-httpd` serves `GET /api/v1/status?timeout=N` (`modules/http-server/src/handler.cpp`).
It registers ds watches on a small set of **bump/edge keys** — `cell.version`,
`gps.version`, `sms.version`, `vpn.state`, `iot.conn.state`,
`services.stats.version`, `log.version`, `iot.update.state`, `cloud.update.status`,
`iot.sensor.version` — then blocks on a condition variable up to `timeout`
seconds. A `NotifyEvent` on any of them wakes it; it then does one bulk `get` of
~180 keys, builds a nested JSON snapshot (`lwm2m/vpn/wifi/wan/cell/gps/sensor/…`),
and returns it — ending the long-poll. On timeout it returns the snapshot anyway.

The Angular `DataStoreService` keeps a permanent long-poll open and republishes
each snapshot to a `BehaviorSubject`; components `observeStatus().subscribe(...)`.

```mermaid
sequenceDiagram
  autonumber
  participant DM as Daemon (e.g. cellular-client)
  participant DS as ds-server
  participant H as iot-httpd (/status)
  participant SVC as DataStoreService
  participant UI as Component (cellular-status)

  UI->>SVC: ngOnInit → observeStatus().subscribe
  SVC->>H: GET /api/v1/status?timeout=30  (startWatch → poll)
  H->>DS: RegisterWatch [cell.version, gps.version, vpn.state, ...]
  H->>H: cv.wait_for(30s, fired)

  DM->>DS: Op::Set {cell.state, cell.version=N+1}
  DS-->>DM: Response(Ok)
  DS-->>H: NotifyEvent {k:"cell.version", v:"N+1"}
  H->>H: notify() → fired=true → cv.notify_one()
  H->>DS: get({~180 keys})
  DS-->>H: values
  H-->>SVC: 200 { ok:true, cell:{state,...}, vpn:{...}, ... }
  SVC->>SVC: ingestStatus(s) → statusSubject.next(s)
  SVC-->>UI: snapshot → this.c = s.cell → re-render datagrid
  SVC->>H: GET /api/v1/status?timeout=30   (re-arm)
```

**Config writes** go the other way: the UI calls `ds.write([{key,value}...])` →
`POST /api/v1/db/set` → `ds-server Op::Set`. Example: the Send-SMS box writes
`sms.send.to` + `sms.send.text` + bumps `sms.send.request`, which the daemon's
watch (step 3 above) is waiting on.

---

## 3. Vehicle telemetry: ingest → storage

**Ingest.** `iot-vehicled` (`modules/vehicle/`) opens a raw SocketCAN socket on
`can0`, round-robins Mode 01 PID requests on the functional id `0x7DF`, decodes
ECU replies (`0x7E8..0x7EF`) with the pure `obd_pid` core, and publishes each
signal **volatile** to `vehicle.*`. DTCs (Mode 03, single-frame only) go to the
**persistent** `vehicle.dtc`. GPS position comes separately from `cellular-client`
(`gps.lat/lon`).

**Storage is two-tier, and the live path is server-Reads — not Send:**

```mermaid
sequenceDiagram
  autonumber
  participant ECU as ECU (OBD-II)
  participant VD as iot-vehicled
  participant DS as device ds-server
  participant LC as lwm2m client (Objects 6, 33000)
  participant DM as cloud lwm2m-dm
  participant CDS as cloud ds-server
  participant HC as cloud iot-httpd
  participant ING as iot-telemetry-ingest
  participant MG as Mongo (iot.telemetry)

  loop every vehicle.poll.interval.ms
    VD->>ECU: CAN 0x7DF Mode 01 PID
    ECU-->>VD: 0x7E8 response
    VD->>DS: set_volatile vehicle.speed/rpm/coolant/...
  end
  Note over LC,DS: client reads vehicle.* → Object 33000 rids, gps.* → Object 6
  loop DM telemetry tick (token-tagged Reads)
    DM->>LC: CoAP Read /6/0/0, /6/0/1  (DTLS :5683)
    LC-->>DM: lat, lon
    DM->>LC: CoAP Read /33000/0/0..8, /33000/0/10
    LC-->>DM: speed,rpm,...,dtc,link
    DM->>CDS: set_volatile cloud.vehicle.telemetry (JSON array of endpoints)
  end
  HC->>CDS: watch cloud.vehicle.telemetry
  CDS-->>HC: NotifyEvent
  HC->>HC: append NDJSON → /var/lib/iot/telemetry-spool/spool.ndjson
  ING->>ING: mongoimport spool.ndjson
  ING->>MG: insert {ts, endpoints:[{endpoint,lat,lon,speed,...}]}
```

### Device-side store-and-forward buffer (the v2 Send path — **gated off**)

The on-device Mongo buffer in the original TDD was **rejected** in favour of a
SQLite outbox, **`DurableSampleBuffer`** (`apps/inc/lwm2m_durable_sample_buffer.hpp`),
which the LwM2M-Send `Uploader` drains. It exists and is unit-tested but is
**off by default** (`iot.telemetry.send.enable=false`) and the **cloud persist for
a Send report is a stub** (`onSendReport` just logs), so nothing is stored via
Send today.

```
outbox( seq  INTEGER PRIMARY KEY AUTOINCREMENT,
        ts   INTEGER,              -- llround(timeUnix * 1000)
        body BLOB,                 -- Sample as compact JSON {t, v:[[name,val]...]}
        sent INTEGER DEFAULT 0 )   -- 0=queued, 1=leased; WAL, synchronous=NORMAL
```

Semantics: `push`→INSERT (+evict over cap); `take(n)`→**lease** oldest n
(`sent=1`, not deleted); `commit()`→DELETE on 2.04 ACK; `requeue()`→un-lease on
timeout; `reap_expired()`→TTL delete. On open, leased rows re-arm to `sent=0` →
**at-least-once** (cloud dedups by `(endpoint, seq)`).

### Cloud collection

Db `iot`, collection **`telemetry`** (`mongo:5.0`, opt-in `telemetry` compose
profile). One document per poll cycle:

```json
{ "ts": 1783726848,
  "endpoints": [
    { "endpoint":"000000006556e041", "lat":12.97,"lon":77.59,
      "speed":42,"rpm":1800,"coolant":88,"throttle":18,"load":34,
      "fuel":61,"iat":31,"maf":12.4,"link":"up","dtc":"" } ] }
```

> **Divergences from the TDD:** the cloud collection is a **plain** collection via
> `mongoimport` — *not* a native time-series with `expireAfterSeconds`; retention
> is a separate `iot-archiver` script (dump → verify → prune). ISO-TP multi-frame
> DTCs (`obd/isotp.cpp`) are **not** implemented (single-frame only). `iot-mqttd`
> has **no** source/unit in-tree and is not on the telemetry path.

---

## 4. How a vehicle is tracked on the map

Position originates from the modem GNSS, is exposed as **LwM2M Object 6
(Location)**, server-Read by `lwm2m-dm`, merged into `cloud.vehicle.telemetry`
alongside the Object-33000 signals, and plotted by the cloud-UI **Leaflet** map.

```mermaid
sequenceDiagram
  autonumber
  participant CC as cellular-client (GNSS)
  participant DS as device ds-server
  participant LC as lwm2m client (Object 6)
  participant DM as cloud lwm2m-dm
  participant CDS as cloud ds-server
  participant MAP as cloud-ui map.component (Leaflet)
  participant TS as tileserver

  CC->>DS: set gps.lat / gps.lon (volatile)
  Note over LC: LocationHooks bind gps.* → Object 6 rids 0/1/5/6
  DM->>LC: CoAP Read /6/0/0 (tag 0x08), /6/0/1 (tag 0x09)
  LC-->>DM: lat, lon
  DM->>CDS: set_volatile cloud.vehicle.telemetry [{endpoint,lat,lon,speed,...}]
  loop every 5s
    MAP->>CDS: POST /api/v1/db/get ["cloud.vehicle.telemetry"]
    CDS-->>MAP: JSON array
    MAP->>MAP: L.circleMarker per endpoint (popup: speed/rpm/coolant/...)
  end
  MAP->>TS: GET /styles/basic/{z}/{x}/{y}  (basemap tiles)
  Note over MAP: history track → GET /api/v1/cloud/telemetry/history?ep=<ep> → L.polyline
```

- **Live markers:** `map.component.ts` long-polls `cloud.vehicle.telemetry` every
  5 s and draws a `L.circleMarker` per endpoint with a fix.
- **History track:** `GET /api/v1/cloud/telemetry/history?ep=<ep>` →
  `L.polyline` (served from `history.json` produced by the ingest sidecar's
  `mongoexport`).
- **Endpoints → Map:** the Endpoints datagrid links to the map via a `?ep=`
  focus param. Note `cloud.endpoints` rows do **not** carry lat/lon (that TDD idea
  was not implemented); position lives only in `cloud.vehicle.telemetry`.

---

## 5. Device-UI access over the VPN (path-scoped reverse proxy)

The operator opens a device's **own web UI** from the cloud Endpoints page
("Launch UI"). The live mechanism is a **same-origin path-scoped reverse proxy**
(`modules/http-server/src/handler_proxy.cpp`, design:
`apps/docs/tdd-device-ui-path-proxy.md`): the cloud `iot-httpd` serves
`/dev/<endpoint>/…` and forwards each request over the OpenVPN tun to the
device's `iot-httpd`. It replaced the per-device published-port + nftables DNAT
approach (`iot-cloudd` still installs the `ip iot_cloud_dnat` DNAT table —
`cloud:<proxy_port> → dev_tun_ip:<ui_port>` — as the direct-port route, shown in
the Endpoints row expander).

Three rewrites make the device SPA (built with `base href "/"`, relative asset
URLs) work under the prefix with **no per-device rebuild**: request-path strip,
`base href` inject, and `Set-Cookie Path` scoping (per-device cookie isolation).
`iot-httpd` shares `iot-cloudd`'s network namespace so `tun0` routes are visible
to its upstream connect.

```mermaid
sequenceDiagram
  autonumber
  participant B as Browser (operator)
  participant HC as cloud iot-httpd (handler_proxy)
  participant CDS as cloud ds-server
  participant CD as iot-cloudd
  participant OS as openvpn server (tun0)
  participant OC as device openvpn-client
  participant HD as device iot-httpd (:8080)

  rect rgb(240,245,250)
    Note over OC,CD: pre-condition — tunnel up + registry knows the device IP
    OC->>OS: TCP :1194 TLS handshake (client cert from LwM2M Object 2048)
    OS-->>OC: assign tunnel IP (e.g. 10.9.0.2), keepalive ping 10
    CD->>OS: mgmt 127.0.0.1 "status" (poll_vpn_client_ips)
    OS-->>CD: ROUTING TABLE vip,CN,real-addr (stale Last Ref > 30s dropped)
    CD->>CDS: sync_endpoints_to_ds → cloud.endpoints[ep].dev_tun_ip = 10.9.0.2
    Note over B: Endpoints page shows "Launch UI" only when dev_tun_ip is set,<br/>launchUrl(ep) = /dev/ + encodeURIComponent(ep) + /
  end

  B->>HC: GET /dev/{ep}/  (same cloud origin, cloud session cookie rides along)
  HC->>HC: auth gate — extract_session_cookie + validate
  alt no / invalid cloud session
    HC-->>B: 302 Location /webui/  (bounce to cloud login)
  end
  HC->>HC: split /dev/{epseg}/{tail}, url_decode(epseg) → ep,<br/>prefix = /dev/{epseg}/
  HC->>CDS: get cloud.endpoints → resolve_dev_tun_ip(ep)
  alt no dev_tun_ip (tunnel down)
    HC-->>B: 502 "device tunnel is down (no route to ep)"
  end
  HC->>CDS: get cloud.proxy.device.ui.port (default 8080)
  HC->>HC: build upstream request — "{method} {tail} HTTP/1.1",<br/>Host: dev_tun_ip, drop hop-by-hop headers,<br/>strip_cookie(cloud session — token never leaves the cloud),<br/>Connection: close (+ Content-Length for body)
  HC->>OS: ACE_SOCK_Connector connect dev_tun_ip:8080 (5s timeout)<br/>— routed via tun0 (shared netns with iot-cloudd)
  OS->>OC: encrypted tunnel frame (TCP :1194)
  OC->>HD: 10.9.0.2:8080 — plain HTTP on the device loopback side
  HD-->>OC: response (index.html / asset / API JSON)
  OC-->>OS: back through the tunnel
  OS-->>HC: bytes until EOF (recv timeout 75s > device long-poll)
  alt connect/timeout failure
    HC-->>B: 504 "device UI did not respond"
  end
  HC->>HC: rewrite response —<br/>1. Set-Cookie: Path=/ → Path=/dev/{epseg}/ (device cookie isolated per device)<br/>2. Location: /x → /dev/{epseg}/x (server-relative redirects)<br/>3. HTML only: base href "/" → "/dev/{epseg}/" (relative assets resolve back)
  HC-->>B: status + rewritten headers/body

  Note over B,HD: SPA boots under the prefix — every asset fetch, device login<br/>(POST /dev/{ep}/api/v1/auth/login → device session cookie scoped to /dev/{ep}/)<br/>and the device /status long-poll repeat steps 8-22 per request<br/>(store-and-forward: no WebSocket/SSE through this proxy)
```

Minute details worth knowing:

- **SSRF-safe by construction:** the upstream target is only ever a
  `dev_tun_ip` looked up from `cloud.endpoints` (written by `iot-cloudd` from
  the OpenVPN management plane) on a fixed port — the URL path cannot steer the
  proxy to an arbitrary host.
- **Two cookie jars, cleanly separated:** the operator's **cloud** session
  cookie is validated at the gate and then **stripped** before forwarding
  (`strip_cookie`), so it never reaches the device; the **device's** session
  cookie comes back with `Path=/dev/<epseg>/`, so N devices can be logged in
  simultaneously from one browser without collisions.
- **Reachability = tunnel, not registration:** Launch UI follows `dev_tun_ip`
  only. The proxy works while the tunnel is up even if the device's LwM2M
  registration has lapsed (registration is shown separately in the State
  column).
- **Store-and-forward:** `upstream_exchange` buffers the full upstream response
  until EOF (`Connection: close` per request). That is why the device-ui
  **Terminal** feature runs on long-poll — WebSocket/SSE cannot pass this
  proxy.
- The `75 s` upstream recv timeout is deliberately longer than the device's
  `/api/v1/status?timeout=30` long-poll, so a held poll completes through the
  proxy instead of 504ing.

---

## 6. VPN plane: certificate delivery + tunnel bring-up (server ⇄ client)

Two flows, in order. **First** the cloud mints and delivers the cert family
(CA cert + client cert + client key) to the device over LwM2M **Object 2048** —
the device generates nothing. **Then** the device's `openvpn-client` dials the
cloud's OpenVPN server, and the server hands it the tunnel network config
(IP, netmask, gateway, DNS, keepalive) in the TLS-protected **PUSH_REPLY**,
which the `openvpn` process itself installs on `tun0`.

### 6.1 PKI: how the CA cert, client cert and private key reach the device

Minting: `CertAuthority` (`modules/server/openvpn/cert_authority.cpp`) runs
entirely on `iot-cloudd` — `ensure()` creates/restores the CA
(`/etc/iot/vpn/ca/ca.key` never leaves the cloud), `mint_client(cn)` runs
`openssl genrsa → req (CSR) → ca` (CA-sign, 10 y) with `cn =
rpi{serial}@cloud.local`. Delivery: `apps/src/lwm2m_dm_server.cpp
build_cert_push` + the chunk codec `apps/inc/lwm2m_cert_chunk.hpp`; install:
`apps/src/lwm2m_object_cert.cpp`.

```mermaid
sequenceDiagram
  autonumber
  participant OP as operator (Endpoints page)
  participant CD as iot-cloudd (CertAuthority)
  participant CDS as cloud ds-server
  participant DM as lwm2m-dm (30s tick)
  participant LC as device lwm2m client (Object 2048)
  participant FS as device /etc/iot/vpn
  participant OC as openvpn-client supervisor

  OP->>CDS: set cloud.provision.request = {serial}
  CDS-->>CD: NotifyEvent
  CD->>CD: ensure() — CA key+cert (restored from ds if present),<br/>mint_client(rpi{serial}@cloud.local) — genrsa → CSR → CA-sign (10y)
  CD->>CDS: cloud.endpoint.credentials += {vpn.client.cert, vpn.client.key}<br/>cloud.vpn.ca.crt.pem (write-only, gid cloud-svc)

  loop every DM tick while endpoint registered AND not in cloud.vpn.connected
    DM->>CDS: get cloud.endpoint.credentials + cloud.dm.uri +<br/>cloud.vpn.listen.port + cloud.vpn.proto
    DM->>DM: build_cert_push — per artifact (ca / cert / key)<br/>certchunk::encode — deflate if PEM > 1024 B,<br/>split into chunks of ≤ 1019 B data + 5 B header<br/>(flags bit0=zipped, seq u16, total u16) —<br/>a whole PEM never fits one DTLS record (tinydtls max 1400)
    DM->>LC: WRITE /2048/0/1 chunk 1..N (opaque, CA cert)
    DM->>LC: WRITE /2048/1/1 chunk 1..N (client cert)
    DM->>LC: WRITE /2048/2/1 chunk 1..N (client PRIVATE key — over DTLS-PSK)
    DM->>LC: WRITE /2048/0/5,6,7 (text) — vpn host (from cloud.dm.uri),<br/>port (cloud.vpn.listen.port), proto (tcp-server → tcp-client)
    DM->>LC: EXECUTE /2048/0/3 (Apply)
    LC->>LC: Reassembler.feed per chunk (idempotent — re-pushed<br/>chunks self-heal), on last chunk inflate → staged PEM
    alt family incomplete (a chunk still missing)
      LC-->>DM: Apply → deferred (logged), next tick re-pushes
    else family + endpoint unchanged vs what is on disk
      LC-->>DM: Apply → skipped (no openvpn bounce on a no-op re-push)
    else fresh family
      LC->>FS: write ca.crt 0644, client.crt 0644, client.key 0640 group iot<br/>(dir 2750 engineer:iot — DynamicUser openvpn-client reads via<br/>SupplementaryGroups=iot, only lwm2m client writes)
      LC->>CDS: set vpn.remote.host / vpn.remote.port / vpn.remote.proto<br/>(device ds — tunnel target now fully cloud-provisioned)
      LC->>OC: gate-flip services.openvpn.client.enable false → true
      Note over OC: supervisor respawns openvpn with the new certs (§6.2)
    end
  end
  Note over DM,OC: stop signal = cloud.vpn.connected lists this endpoint<br/>(iot-cloudd reads it from the OpenVPN mgmt plane) — proves the cert<br/>WORKS, not just landed. A re-mint drops the tunnel → push resumes.
```

> **Security shape:** the client private key is cloud-generated and exists in
> three places — cloud ds (write-only ACL), the DTLS-PSK-encrypted LwM2M wire,
> and device disk (`0640` group `iot`). The CA **key** is cloud-only; devices
> only ever receive the CA **cert**. Revocation = `openssl ca -revoke` → CRL →
> `cloud.vpn.crl.pem`, enforced by the server's `crl-verify`.

### 6.2 Tunnel bring-up: what the server sends and how tun0 gets configured

Server config (`modules/server/openvpn/openvpn_server.cpp
build_server_config`): `mode server`, `tls-server`, `topology subnet`,
`server 10.9.0.0 255.255.255.0` (the ifconfig-pool), optional per-client
`client-config-dir` (`ifconfig-push` static IPs, multi-tenant), `push
"dhcp-option DNS …"`, `crl-verify`, `dh none` (ECDH), `keepalive 10 60`,
`management 127.0.0.1 {mgmt_port}`. Client config
(`modules/openvpn/client/src/process.cpp build_openvpn_config`): `client`,
`dev tun0`, `proto tcp-client`, `remote {host} {port}`, `nobind`,
`resolv-retry infinite`, `persist-tun`, `persist-key`, `ca/cert/key
/etc/iot/vpn/*`, `management 127.0.0.1 {port}`, **`management-hold`**.

```mermaid
sequenceDiagram
  autonumber
  participant SUP as openvpn-client supervisor (daemon)
  participant DDS as device ds-server
  participant OV as openvpn process (device)
  participant TUN as tun0 (device kernel)
  participant OS as openvpn server (cloud :1194)
  participant CD as iot-cloudd

  SUP->>DDS: gates — services.openvpn.client.enable,<br/>dep health net.router, target iface up (Gate.evaluate)
  SUP->>DDS: snapshot vpn.remote.host/port/proto, vpn.mgmt.port, cipher
  SUP->>SUP: build_openvpn_config → mkstemps /tmp/openvpn-XXXXXX.conf,<br/>spawn openvpn (held — mgmt socket open, NOT dialing yet)
  SUP->>OV: mgmt attach 127.0.0.1 — "state on" + "state 1" (query current —<br/>fixes the missed-CONNECTED race), "log on" (PUSH_REPLY rides the log),<br/>"hold release"
  SUP->>DDS: vpn.state = connecting

  OV->>OS: TCP connect :1194, TLS handshake —<br/>client presents client.crt, verifies server against ca.crt
  OS->>OS: verify client cert against CA + crl-verify (revoked → reject),<br/>cert CN rpi{serial}@cloud.local identifies the device
  OV->>OS: PUSH_REQUEST
  OS-->>OV: PUSH_REPLY — route-gateway 10.9.0.1,<br/>ifconfig 10.9.0.2 255.255.255.0 (topology subnet auto-push,<br/>or the CCD ifconfig-push static), dhcp-option DNS {x},<br/>ping 10, ping-restart 60 (from server keepalive 10 60)
  OV->>TUN: openvpn itself installs — open /dev/net/tun ioctl TUNSETIFF →<br/>tun0 up, addr 10.9.0.2/24, route 10.9.0.0/24 dev tun0<br/>via route-gateway (NO redirect-gateway — only tunnel subnet routed)
  OV-->>SUP: mgmt ">STATE ... CONNECTED,SUCCESS,10.9.0.2"
  SUP->>DDS: vpn.state = connected, vpn.assigned.ip = 10.9.0.2
  OV-->>SUP: mgmt ">LOG PUSH: Received control message PUSH_REPLY,..."
  SUP->>DDS: parse ifconfig / route-gateway / dhcp-option DNS →<br/>vpn.assigned.netmask / .gateway / .dns (device-ui VPN page shows these)

  Note over OV,OS: keepalive ping every 10s each way, peer declared dead<br/>after 60s (server reaps a hard-off client in ~120s)
  CD->>OS: mgmt "status" poll — ROUTING TABLE vip,CN,real-addr
  CD->>CD: cloud.vpn.connected += endpoint, dev_tun_ip = 10.9.0.2<br/>(stops the §6.1 cert re-push, enables §5 Launch UI + DNAT)

  Note over SUP,OV: reconnects — openvpn re-dials on its own (resolv-retry,<br/>persist-tun/persist-key keep tun0 + keys), each cycle re-emits >HOLD:<br/>and the supervisor re-releases it. Any vpn.* ds change = hot-reload:<br/>tear down + respawn with a freshly rendered config.
```

Minute details worth knowing:

- **State names on the wire → `vpn.state`:** the mgmt `>STATE:` codes are
  normalised (`Lifecycle::normalise_state`) — `CONNECTING/TCP_CONNECT →
  connecting`, `RESOLVE → resolving`, `WAIT/GET_CONFIG → wait`, `AUTH → auth`,
  `ASSIGN_IP/ADD_ROUTES → connecting`, `CONNECTED → connected`, `EXITING →
  exited`.
- **Why `management-hold` + `state 1`:** openvpn is spawned *held* so the
  supervisor can attach and enable notifications **before** the first dial —
  otherwise the CONNECTED transition and PUSH_REPLY fire before anyone is
  subscribed and `vpn.state` stalls at "connecting" forever (a real field bug).
  `state 1` additionally queries the *current* state on (re)attach.
- **PUSH_REPLY arrives via the log**, not a mgmt `>PUSH_REPLY` event (most
  openvpn builds don't emit one) — hence `log on` and the
  `PUSH: Received control message` parse.
- **The assigned IP has two sources:** field 4 of the `CONNECTED` state line
  and the `ifconfig` push option — both feed `vpn.assigned.ip`.
- **No default-route hijack:** nothing pushes `redirect-gateway`, so only
  `10.9.0.0/24` rides the tunnel. LwM2M/DTLS (`:5683`) and the OTA download
  stay on the WAN — the VPN is for operator access (device-UI proxy, DNAT),
  not the telemetry plane.
- **LwM2M reacts to the tunnel:** the lwm2m client watches `vpn.state` and
  re-Registers on a reconnect — fast recovery after a cloud restart.

---

## 7. LwM2M plane: bootstrap & DM registration

The control plane is **direct device→cloud DTLS over UDP** (no VPN): bootstrap
on `:5684`, device management on `:5683`. The device is flashed with only its
serial + BS PSK; everything else — the DM account (URI, identity, key),
registration lifetime, binding — is **delivered by the Bootstrap Server** and
committed atomically. Code: client `apps/src/lwm2m_bootstrap_client.cpp` +
`lwm2m_registration_client.cpp`, server `apps/src/lwm2m_bootstrap_server.cpp` +
`lwm2m_registration_server.cpp` (run in `role=server` as the `lwm2m-bs` /
`lwm2m-dm` containers), account synthesis `modules/server/lwm2m/` +
`cloud_credentials.cpp`.

### 7.1 Bootstrap (`/bs`): from factory identity to a DM account

Identities are **derived, never stored**: the BS DTLS identity is
`sha256(serial)[:32]` (computed identically on both ends), the DM identity is
`rpi{serial}@cloud.local` (formatted by the cloud). The device ships with just
`iot.serial` (auto-filled on RPi) + `iot.bs.psk.key` (commissioned via
device-ui, or flash-time HKDF-personalised — zero-touch tier).

```mermaid
sequenceDiagram
  autonumber
  participant LC as lwm2m client (device)
  participant DDS as device ds-server
  participant BS as lwm2m-bs (:5684)
  participant CDS as cloud ds-server

  LC->>DDS: startup — get iot.serial (= endpoint), iot.bs.uri,<br/>iot.bs.psk.key (+ iot.bs.psk.override for 3rd-party BS)
  LC->>LC: derive BS DTLS identity = sha256(serial)[:32]<br/>(override on → operator-pinned iot.bs.psk.identity instead)
  LC->>DDS: iot.conn.state = bootstrapping
  LC->>BS: DTLS-PSK handshake — ClientHello ... identity in ClientKeyExchange
  BS->>CDS: PSK resolver (live, per-handshake) — match sha256(serial) against<br/>cloud.endpoint.credentials, fallback identity / dm.psk.id forms,<br/>else HKDF tier — derive(master, "iot-bs-psk:v1:" + serial)
  BS-->>LC: handshake complete (session keys established)

  LC->>BS: POST /bs?ep={serial}  (CON, Bootstrap-Request)
  BS-->>LC: 2.04 Changed  (state → WaitForBSWrites)
  BS->>BS: provisioning_resolver(ep) — synthesise AccountProvisioning from<br/>cloud.bs.* + cloud.dm.* + the endpoint's credentials row<br/>(stored row wins, else HKDF-derive — stateless DM)
  BS->>LC: PUT /0/0 (TLV, Security iid 0 = the BS account)<br/>RID0 bs uri, RID1 bootstrap=true, RID2 mode=PSK,<br/>RID3 sha256(ep)[:32], RID5 bs key, RID10 ssid=0
  LC-->>BS: 2.04 (staged, not yet live)
  BS->>LC: PUT /0/1 (TLV, Security iid 1 = the DM account)<br/>RID0 coaps://{cloud.dm.uri}:5683, RID1 false, RID2 PSK,<br/>RID3 rpi{serial}@cloud.local, RID5 dm key, RID10 ssid=1
  LC-->>BS: 2.04 (staged)
  BS->>LC: PUT /1/1 (TLV, Server object)<br/>RID0 ssid=1, RID1 lifetime (cloud.dm.lifetime, default 90 s),<br/>RID7 binding "U"
  LC-->>BS: 2.04 (staged)
  BS->>LC: POST /bs (no payload — Bootstrap-Finish)
  LC->>LC: apply_commit() — ATOMIC — purge/delete staged removals,<br/>install Security + Server instances into the live ObjectStore,<br/>add_credential(dm identity, key) into the DTLS store
  LC-->>BS: 2.04 Changed

  Note over LC,DDS: on_done — persist iot.dm.psk.identity/key (write-only) +<br/>iot.dm.uri, DERIVE vpn.remote.host from the DM URI host<br/>(VPN concentrator co-located — zero device VPN config),<br/>set registration lifetime from the committed Server object
  LC->>LC: switch peer to DM host:5683, active_identity(dm),<br/>reset_and_connect(toBootstrapIdentity=FALSE) — tear down the<br/>stale peer so a PLAINTEXT ClientHello opens the DM handshake<br/>(keeping the DM identity — restoring the BS one here caused the<br/>"stuck at 90% + offline" regression)
  LC->>DDS: iot.conn.state = dm-connecting
```

### 7.2 DM registration (`/rd`): register, heartbeat, recover

```mermaid
sequenceDiagram
  autonumber
  participant LC as lwm2m client (device)
  participant DDS as device ds-server
  participant DM as lwm2m-dm (:5683)
  participant CDS as cloud ds-server
  participant CD as iot-cloudd

  LC->>DM: DTLS-PSK handshake — identity rpi{serial}@cloud.local
  DM->>CDS: PSK resolver — dm.psk.id match in cloud.endpoint.credentials,<br/>else HKDF derive("iot-dm-psk:v1:" + serial) — provisioned-after-start<br/>devices authenticate with NO server restart
  DM-->>LC: session up
  LC->>DDS: iot.conn.state = dm-connected

  LC->>DM: POST /rd?ep={serial}&lt=90&lwm2m=1.1&b=U  (CON)<br/>payload = CoRE link-format of the ObjectStore<br/>("</1/1>,</3/0>,</4/0>,</5/0>,</6/0>,</2048/0>,...")
  DM->>DM: ClientRegistry add — endpoint, lifetime, binding, version,<br/>peer addr (the NAT public ip:port = isp_ip), objects
  DM-->>LC: 2.01 Created, Location-Path /rd/{id}
  LC->>DDS: iot.conn.state = registered
  DM->>CDS: publish_regs → cloud.lwm2m.registrations<br/>[{endpoint, registered, last_seen_unix, lifetime, location, ...}]
  CDS-->>CD: NotifyEvent → reconcile_registrations —<br/>cloud.endpoints[ep] flips online (Endpoints page)

  loop heartbeat — every lifetime − 30 s (updateMarginSeconds)
    LC->>DM: POST /rd/{id}?lt=90  (Update — doubles as the NAT-conntrack<br/>keepalive: lifetime is sized to UDP NAT timeouts, NOT a day)
    DM-->>LC: 2.04 Changed (last_seen refreshed)
  end

  alt lifetime lapses with no Update (device dark)
    DM->>DM: registry expire (per-tick) → RegistrationOutcome Removed
    DM->>CDS: republish (endpoint absent) → cloud.endpoints offline
  else re-register triggers (self-healing)
    Note over LC: vpn.state flip (tunnel reconnect ⇒ cloud likely restarted),<br/>Update ack timeout, endpoint/serial change — each forces a fresh<br/>Register (and a Failed handshake falls back to re-bootstrap:<br/>reset_and_connect back to the BS restores the BS identity)
  end

  opt clean shutdown
    LC->>DM: DELETE /rd/{id} (Deregister)
    DM-->>LC: 2.02 Deleted → registry Removed → cloud.endpoints offline
  end
```

Minute details worth knowing:

- **`iot.conn.state` machine** (`compute_conn_state`, published on change each
  tick): `bootstrapping → bootstrapped → dm-connecting → dm-connected →
  registered`, plus `failed` / `idle`. "-connecting" = DTLS handshake in
  flight, "-connected" = channel up, protocol exchange underway. The device-ui
  and cloud dashboard read exactly this key.
- **Staging is all-or-nothing:** Bootstrap-Writes land in a `StagingBuffer`
  (the live store is untouched); only Bootstrap-Finish `apply_commit()`s —
  a half-delivered bootstrap can't leave a device with a broken mix.
- **Bootstrap-Delete** (`DELETE /` purge or `DELETE /{oid}/{iid}`) is honored
  in staging too — the BS wipes stale accounts before writing fresh ones.
- **Lifetime 90 s is deliberate:** the Update at `lifetime − 30 s` (fixed
  `updateMarginSeconds`) keeps the home-router UDP conntrack mapping alive
  (assured-UDP timeout ≈ 120 s) so the cloud can still reach the device for
  OTA pushes and server-Reads. See the NAT-keepalive table in
  `apps/cloud/CLAUDE.md`.
- **The DM peer address is the device's public IP** — `ClientRegistry`
  captures the Register's DTLS source (`isp_ip` in the Endpoints table),
  VPN-independent.
- **Two hard-won wedge fixes live in this path:** (1) on the DM switch,
  `reset_and_connect` must keep the **DM** identity (`toBootstrapIdentity=
  false`) — restoring the BS identity left devices unregistered after OTA;
  (2) a cloud restart leaves the device's tinydtls peer CONNECTED while the
  server forgot it — the forced fresh handshake (plaintext ClientHello) on
  the DM switch plus the `vpn.state`-triggered re-Register recover it without
  a manual client restart.
- **Re-bootstrap uses the BS identity again:** any fall-back to `/bs` calls
  `reset_and_connect(bsHost, bsPort)` with the default identity restore —
  a device must never offer its DM identity to the BS (the historical
  `bootstrapping`-forever wedge).

---

## 8. OTA update: end to end (feed → push → stage → install → feedback)

Full design: `apps/docs/tdd-yocto-swupdate.md` (.ipk / bundle path) and
`apps/docs/tdd-ab-image-ota.md` (A/B full-image). The chain is deliberately
split into four processes with different privileges: the **unprivileged**
lwm2m client only *queues a request file*; two **root** oneshot units
(`iot-ota-stage` → `iot-swupdate`) do the download and the install, decoupled
by systemd `.path` (inotify) triggers so an OTA that replaces the running
binaries can't kill its own installer.

```mermaid
sequenceDiagram
  autonumber
  participant OP as operator (cloud-ui Software Update)
  participant HC as cloud iot-httpd
  participant CDS as cloud ds-server
  participant CD as iot-cloudd
  participant DM as lwm2m-dm
  participant LC as lwm2m client (device, engineer)
  participant ST as iot-ota-stage (root oneshot)
  participant SW as iot-swupdate (root oneshot)
  participant DDS as device ds-server

  rect rgb(240,245,250)
    Note over OP,CDS: publish to the feed — two ingest paths
    OP->>HC: upload bundle / "fetch from URL" (background download,<br/>progress via a ds key the UI observes)
    HC->>HC: store /var/lib/iot/firmware/{name}, sha256sum it<br/>(extension allow-list — .ipk .tar .tar.gz .tgz .raucb)
    HC->>CDS: upsert cloud.firmware.manifest row<br/>{pkg, version, arch, ipk_url, sha256}
  end

  OP->>CDS: set cloud.update.request {serials[], url, sha256, version, pkg}
  CDS-->>CD: NotifyEvent
  CD->>CD: validate url against the manifest (fills sha/ver/pkg),<br/>resolve relative url — cloud.firmware.base.url, else<br/>http://{host of cloud.dm.uri} — then append ?sha256=...&version=...,<br/>cid = ++cloud.update.seq (persisted — survives restarts)
  CD->>CDS: cloud.update.pending = [{endpoint, cid, url, sha256, version}]<br/>cloud.update.status = [{serial, state 1, result 0, cid,<br/>baseline = installed_version snapshot}]

  DM->>CDS: tick (30 s) — get cloud.update.pending
  DM->>DM: gates — job for this endpoint, cid not already sent<br/>(at-most-once PER CAMPAIGN), downgrade guard: never push<br/>ver ≤ last-read /3/0/3 semver, unknown installed → DEFER
  DM->>LC: WRITE /5/0/1 (Package URI, text) then EXECUTE /5/0/2 (DTLS)
  DM->>DM: drop cached Object-5 readback, stamp update_cid = cid

  LC->>LC: RID 2 execute → ota_launch_apply(uri) — engineer CANNOT<br/>systemd-run a root unit (polkit denies), so — write<br/>/run/iot/update/stage.req.tmp + atomic rename (spool 2775 root:iot,<br/>client has SupplementaryGroups=iot)
  Note over ST: iot-ota-stage.path (inotify on stage.req) starts the service
  ST->>ST: consume stage.req (rm — re-arms the .path),<br/>parse ?sha256 / version / reboot from the URI
  ST->>DDS: iot.update.package = "{name} ({ver})", progress 0,<br/>result 0 + reason "" (fresh campaign starts clean), state 1
  ST->>ST: HEAD Content-Length → background poller publishes<br/>bytes-on-disk / total as progress 0..99 every 1 s
  ST->>HC: curl -C - (resume) with retry/backoff, up to<br/>iot.update.retries (5) attempts — rides the PUBLIC WAN, not the VPN
  alt download exhausts retries / empty file
    ST->>DDS: reason + result 8, state 0 — exit
  end
  ST->>DDS: progress 100
  ST->>ST: sha256sum vs ?sha256=
  alt sha mismatch
    ST->>DDS: reason "sha256 mismatch (want .. got ..)" + result 5 — exit
  end
  ST->>ST: purge stale spool artifacts (a leftover older .ipk set<br/>poisons opkg with cross-version conflicts), extract bundle<br/>(gzip -t content-detect, busybox gzip|tar)
  alt no .ipk in the extracted bundle
    ST->>DDS: reason "no .ipk in bundle" + result 9 — exit<br/>(the 1.5.0 source-tarball incident)
  end
  ST->>DDS: state 2 (downloaded)
  ST->>ST: write update.meta (version/sha/reboot), touch trigger "update"

  Note over SW: iot-swupdate.path (inotify on the trigger) starts the service
  SW->>SW: re-exec from a tmpfs self-copy (survives opkg replacing<br/>this very script), flock (serialize), snapshot *.ipk + meta<br/>into .work-$$, rm trigger (re-arm)
  alt offered semver < running iot.version
    SW->>DDS: reason "downgrade refused (...)" + result 10 (skipped), state 0
  end
  SW->>DDS: state 3 (updating)
  SW->>SW: opkg install --force-reinstall --force-downgrade .work/*.ipk
  alt opkg fails
    SW->>DDS: reason "opkg install failed: {last log line}" + result 9
  end
  SW->>SW: migrations — restart iot-ds (loads new schemas), run<br/>/usr/share/iot/migrations newer than iot.config.version
  SW->>DDS: iot.update.version = {ver}, reason "", result 1, state 0
  alt kernel / base-files / systemd / bootloader in the install set
    SW->>SW: systemctl reboot
  else userspace only
    SW->>SW: daemon-reload + try-restart iot daemons<br/>(including the lwm2m client — the reporter dies here)
  end

  rect rgb(240,245,250)
    Note over LC,CD: completion feedback — two independent signals
    LC->>DM: restarted client re-Registers, DM Reads /3/0/3 →<br/>installed_version moves OFF the row's baseline
    CD->>CDS: reconcile — row state 0, result 1 (SUCCESS — robust to<br/>the +gitsha versioning, no result observe needed)
    DM->>LC: 30 s poll Reads /5/0/3 + /5/0/5 + /5/0/26 (reason)
    DM->>CDS: update_state/result/reason + update_cid in registrations
    CD->>CDS: cid-matched terminal result → row failed + REASON<br/>(FAILURE — a failed install moves no version, this poll is<br/>the only signal, see tdd-yocto-swupdate §3.4b)
  end
```

Minute details worth knowing:

- **Three downgrade gates, deliberately redundant:** cloud push gate (semver vs
  last-read `/3/0/3`, unknown → defer — a blind push right after Register once
  wedged opkg on cross-version deps), device `iot-swupdate` gate (result 10
  "skipped"), and opkg's own `--force-downgrade` is only reachable through the
  first two.
- **The campaign id (`cid`)** is a persisted monotonic counter, not a clock:
  re-pushing even the *same* version re-sends (fresh cid), double-pushes in
  the same second stay distinct, and the Object-5 failure readback is
  attributed to exactly the campaign that was pushed (`update_cid` match).
- **Progress is byte-accurate on the device, phase-based on the cloud:** the
  stager's poller publishes real bytes/total to `iot.update.progress`
  (device-ui); the cloud bar maps Object-5 state → 30/65/90 %.
- **The download rides the WAN, not the VPN** — the manifest URL resolves
  against the cloud's *public* base, so a device with a down tunnel still
  updates (and the feed is https:443 — an empty `base.url` once produced a
  dead `http://…:80` URL).
- **Everything in `/run/iot/update` is tmpfs** — a reboot wipes half-staged
  campaigns; the spool's `2775 root:iot` mode is restored by both scripts
  because a root `mkdir` under umask once locked the engineer client out of
  writing the *next* `stage.req` (EACCES → campaign hangs).
- **A/B variant:** a `.raucb` in the spool takes the rauc branch instead of
  opkg — `rauc install` writes the **inactive** bank, reboot activates it, the
  bootloader's boot-attempts counter rolls back unless `iot-ota-confirm`
  health-checks and marks the boot good (`iot.boot.bank/banks/confirmed`,
  shown on the device-ui Software page). Atomic and power-fail-safe, unlike
  the in-place opkg path.
- **`iot.update.request` is the third entry point:** device-ui (or a direct
  Object-5 write) can set a URL locally — same stager path, no cloud campaign
  row. Drag-and-drop upload on the device-ui skips the stager entirely and
  drops the artifact straight into the spool.

---

## 9. Schema summary

### Device ds keys (`modules/vehicle/schemas/vehicle.lua`, `.../cell.lua`)

| Key | Type | Persist | Set by | Consumed as |
|---|---|---|---|---|
| `vehicle.can.iface` / `.bitrate` / `.poll.interval.ms` | string/int | yes | operator | daemon config |
| `vehicle.speed rpm coolant throttle load fuel iat maf` | string | **volatile** | iot-vehicled | Object 33000 rid 0–7 |
| `vehicle.dtc` | string | **yes** | iot-vehicled | Object 33000 rid 8 |
| `vehicle.link` | string | volatile | iot-vehicled | Object 33000 rid 10 |
| `gps.lat lon alt speed` | string | volatile | cellular-client | Object 6 rid 0/1/2/6 |
| `iot.telemetry.send.enable` (+6) | mixed | yes | operator | Send Uploader (gated) |

### LwM2M objects

| Object | Name | RIDs |
|---|---|---|
| **6** | Location | 0 lat, 1 lon, 2 alt, 5 ts, 6 speed |
| **33000** | Vehicle telemetry (private) | 0 speed … 7 maf, 8 dtc, 10 link |

### Cloud

| Where | Name | Shape |
|---|---|---|
| ds key | `cloud.vehicle.telemetry` (volatile) | JSON array of `{endpoint,lat,lon,<signals>,link,dtc}` |
| Mongo | db `iot`, coll `telemetry` | `{ts, endpoints:[{endpoint,lat,lon,<signals>}]}` |

---

## 10. Prerequisites (end-to-end)

**Device**
- `ds-server` running (daemons exit if `connect` fails).
- CAN up: `iot-can0-up.service` (`ip link set can0 up type can bitrate 500000`),
  kernel CAN controller driver (e.g. `mcp251x`) + `can`/`can-raw` modules.
- `iot-vehicled` (`CAP_NET_RAW`, `After=iot-ds iot-can0-up`), and a vehicle/ECU on
  the bus answering PIDs (else `vehicle.link=no-ecu`).
- `cellular-client` with a **GNSS fix** for map position (`gps.lat/lon`).
- `lwm2m` client **registered** to the cloud DM over direct DTLS `:5683` — this is
  what lets `lwm2m-dm` server-Read Objects 6 & 33000. **VPN is not required** for
  telemetry (direct DTLS plane).

**Cloud**
- `lwm2m-dm` (issues the token-tagged Reads → `cloud.vehicle.telemetry`).
- `iot-httpd` (spools telemetry + serves `/api/v1/cloud/telemetry/history` + UI).
- **`telemetry` compose profile** enabled for history + basemap: `mongo`,
  `tileserver`, `iot-telemetry-ingest`, `iot-archiver`. Without it, **live markers
  still work** (they read the volatile ds key); history/track/charts and tiles do
  not.

**For the (incomplete) LwM2M Send v2 path** — not needed for current map/history:
- `iot.telemetry.send.enable=true` + `iot.telemetry.db.path` set.
- Blocked on: client session-I/O glue on HW, cloud Send persist (currently a
  log-only stub), and full RFC 7959 Block-Wise (only partial in the adapter).

---

_See also: `apps/docs/tdd-vehicle-telemetry.md` (design), `apps/docs/lwm2m-design.md`._
