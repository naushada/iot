# TDD Plan — Serial-derived Endpoint & Write-only PSK Provisioning

Status: **COMPLETE** (implementable scope) — merged via PR #101 (+ #102).
Device side end-to-end + cloud side through the Endpoints provision UI are
implemented and unit/build-verified in podman (apps 178/178, ds PSK 17/17,
cloud-cred 10/10, both UIs build, full `lwm2m` builds). Build/test via
**podman**. Cloud server account = `cloud-svc`; provisioning is **db/set**-
driven (no bespoke REST endpoint).

**Closed out.** The only remaining work is the live-stack integration pass
(needs a running BS+DM+device together) — tracked below as N-wire, P
(container non-root), and K (separate-DM-peer swap). These are deferred
deliberately: they cannot be meaningfully verified without the integration
environment.

### Implementation progress (auto mode)

| Task | State | Notes |
| --- | --- | --- |
| C — read_acl enforcement + dev-mode bypass | ✅ DONE | `check_read_acl` in schema.{hpp,cpp} (shared `acl_matches` helper); Get + RegisterWatch gated in worker.cpp; lazy `dev_mode_on()` honours `iot.dev.mode`/`cloud.dev.mode`. Unit + integration tests green in podman. |
| D — new schema keys | ✅ DONE | 6 keys added to `schemas/iot.lua`; `Schema.real_iot_lua_declares_psk_keys` asserts types + ACLs. |
| J — engineer account + de-DynamicUser + socket group | ✅ MOSTLY | `iot-sysusers.conf`; client unit → `User=engineer`/`Group=engineer`/`SupplementaryGroups=iot`; ds unit → `ds-socket-group=iot` + `SupplementaryGroups=iot`; ds-server gained `ds-socket-group=` (server.{hpp,cpp} chgrp, `PSK_socket_group_is_applied` test green); yocto `useradd` in `iot_git.bb` + mirror units updated. **J4 (on-device boot verify) pending real hardware.** |
| I — Restart=always | ✅ DONE | both client units set `Restart=always`. |
| A — RPi serial reader | ✅ DONE | `apps/{inc,src}/rpi_serial.{hpp,cpp}` (`read_rpi_serial`, `is_rpi`); 6 tests in `apps/test/rpi_serial_test.cpp`. Full apps suite 165/165 green in podman. |
| B — PSK generator (C++) | ✅ DONE | `apps/{inc,src}/psk_gen.{hpp,cpp}` (`generate_psk_hex`/`hex_encode`/`hex_decode`); 5 tests. For cloud DM PSK (task M). |
| E,F,G,K policy logic | ✅ DONE | `apps/{inc,src}/provisioning_policy.{hpp,cpp}` — `resolve_endpoint`, `should_restart_on_psk_change`, `is_coap_client_error`, `should_rebootstrap`; 13 tests. |
| DsConfig extension | ✅ DONE | PSK accessors (serial/bs/dm/dev_mode) + writers (`set_serial`, `set_dm_credentials`) + watches all keys. Syntax-checked + links in full `lwm2m`. |
| E — client wiring | ✅ DONE | main.cpp: resolve endpoint, RPi serial auto-fill, BS PSK (identity=serial, key=iot.bs.psk.key) into DTLSAdapter; CLI args remain fallback. Full `lwm2m` builds in podman. |
| F — DM write-back | ✅ DONE | bootstrap `on_done` persists DM identity + hex(secretKey) → `iot.dm.psk.*` via `set_dm_credentials`. |
| G — self-restart on PSK change | ✅ DONE | `on_change(BsPskKey)` → `should_restart_on_psk_change` → `::exit(0)` (systemd Restart=always). |
| K — re-bootstrap on DM reject | ✅ MOSTLY | tick: reg `Failed` + `should_rebootstrap` + 30s cooldown → re-POST `/bs`. Correct for shared BS+DM peer; **separate-DM-peer needs a peer swap back to BS (integration P2)**. |
| H — device-ui (Angular) | ✅ DONE | `lwm2m-config` gains serial field (RPi read-only / installer entry) + dev-mode-gated "Generate BS PSK" (browser `crypto.getRandomValues`, shown once, write-only `dbSet`). **iot-ui production build green in podman (node:18).** |
| L — cloud schema | ✅ DONE | `cloud.endpoint.credentials` (JSON array, `gid:cloud-svc` ACLs) + `cloud.dev.mode` in `cloud.lua`. |
| M — cloud provision (helper + wire) | ✅ DONE | `cloud_credentials` helper (`format_identity`, `upsert`/`remove`, `credentials_for_instance`, `generate_psk_hex`) — 10 tests. iot-cloudd provision watch: reads `cloud.provision.bs.psk` carrier → mints DM PSK → `upsert_credential` → writes `cloud.endpoint.credentials` → clears carrier (syntax-checked). **db/set-driven, no bespoke endpoint.** |
| N — BS/DM live creds | ✅ CORE | `credentials_for_instance` (BS→raw serial, DM→formatted identity) tested. **N-wire (server-role load into DTLS adapter + watch + bootstrap DM-security-object push) = integration; depends on the in-process BS/DM topology + live device, validated in P2.** |
| O — cloud-ui Endpoints | ✅ DONE | Endpoints tab gains a provision form (serial + BS PSK) → `db/set` (`cloud.provision.bs.psk` + `cloud.provision.request`); reads `cloud.dev.mode`. **cloud-ui prod build green in podman.** |
| P — cloud-svc account | ✅ CORE | Dockerfile creates `cloud-svc` group+user; compose ds-server `ds-socket-group=cloud-svc`. **Flipping the server containers to `user: cloud-svc` (primary gid for SO_PEERCRED) + caps for iot-cloudd/openvpn = integration (P2).** |

Test cmd (apps): needs `apt-get install -y zlib1g-dev` in the image, then
`cmake .. -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 && make -j lwm2m_test && ./lwm2m_test`
(full client: `cmake ../  -DIOT_ENABLE_MONGO=OFF ... && make -j lwm2m`).
Status: apps **178/178** green, ds PSK **17/17** green.

Test cmd (data-store):
`podman run --rm -v "$PWD":/src:Z -w /src/modules/data-store/build localhost/iot-httpd:lp bash -lc 'cmake .. -DBUILD_DATA_STORE_TESTS=ON && make -j ds-tests && ./ds-tests'`
(pre-existing failures: `DEP_*`, `LogBufferTest.*` — in-process-harness, unrelated.)

## 1. Goal

Move DTLS PSK credentials and the LwM2M endpoint out of CLI args / Lua
config and into the data-store, with the following behaviour:

1. On a Raspberry Pi, the lwm2m client reads the hardware serial number at
   startup and writes it to the serial/endpoint key (auto-fill). This runs
   **before the client's provisioning park** (the wait-loop for `iot.bs.uri` +
   BS PSK): the serial is what the operator reads from the device-ui to
   generate the BS PSK and commission the device, so it must be live *before*
   commissioning, not after. On non-RPi the client leaves it empty and the
   **installer** types it in the device-ui Configuration page. The auto-filled
   value is the **raw** device serial (`read_rpi_serial`, device-tree/cpuinfo),
   not the `iot-<serial>` hostname, and `resolve_endpoint` never overwrites an
   already-set serial. The serial is the LwM2M **endpoint** and the DTLS PSK
   **identity (raw, on the wire)** for the Bootstrap (BS) handshake.
2. **device-ui** generates a 32-byte BS PSK key, shows it in an input field
   (so the engineer can copy it), and stores it in the data-store
   **write-only** (no read via CLI, enforced server-side). Generation /
   reveal is **dev-mode gated** — see decision table.
3. The lwm2m client (user `engineer`) reads that PSK to complete the DTLS
   handshake with the BS server, sending the **raw serial** as the PSK
   identity.
4. Endpoint (LwM2M `ep=`) = raw serial number.
5. On non-RPi hardware, the installer enters the serial on the device-ui
   Configuration page; on RPi the field is auto-filled (read-only).
6. The cloud BS server pushes the DM (Device Management) identity + PSK to
   the device during bootstrap. The client stores them in the data-store,
   also write-only / no CLI read. The DM identity is the cloud's formatted
   string `rpi<serial>@cloud.local`.
7. In **dev-mode** the CLI may read/write the PSK keys; if the PSK
   changes, the lwm2m client restarts itself (full process exit, relaunched
   by systemd `Restart=always`).
8. If the **DM server rejects** the client (DTLS auth failure *or* LwM2M
   registration error 4.0x), the client re-runs bootstrap to obtain fresh
   DM credentials.
9. The OpenVPN tunnel is **not** in the LwM2M (BS/DM) path. It is used only
   to proxy the device-hosted device-ui (already in the design doc).

### Identity model (two namespaces)

| Where | Value |
| --- | --- |
| On the wire, BS DTLS PSK identity (device → BS) | **raw serial** (e.g. `100000003d1f9c2e`) |
| LwM2M endpoint name (`ep=`) | **raw serial** |
| Cloud canonical identity (keys both PSKs server-side) | **`rpi<serial>@cloud.local`** |
| On the wire, DM DTLS PSK identity (device → DM) | **`rpi<serial>@cloud.local`** (received during bootstrap) |

The device knows only its raw serial. The cloud owns the formatted
namespace: the BS server reformats the wire serial → `rpi<serial>@cloud.local`
to look up both the BS PSK (to decode the current handshake) and the DM PSK
(to push into the device during bootstrap). Prefix is literally `rpi` for all
hardware (single code path); revisit per-type prefixes later if needed.

### Confirmed design decisions

| Decision | Choice |
| --- | --- |
| Write-only enforcement | **Server-side `read_acl`** in ds-server (currently parsed but unenforced). |
| "Restart itself" | **Full process exit**, relaunched by systemd `Restart=always`. |
| Dev-mode detection | **Data-store key** `iot.dev.mode` (boolean, `write_acl = {"gid:engineer"}`). |
| Serial / endpoint / BS identity (device) | **One value** — raw serial == LwM2M `ep=` == BS DTLS PSK identity on the wire. |
| Cloud canonical identity | **`rpi<serial>@cloud.local`** — keys both BS and DM PSKs server-side; BS server reformats the wire serial to this for lookup. |
| DM PSK identity | **= the formatted string** `rpi<serial>@cloud.local` (same identity for BS and DM; req Q3). |
| PSK generator | **device-ui** generates the 32-byte BS PSK (not the lwm2m client); cloud **generates the DM PSK** at provisioning. |
| device-ui PSK reveal | **dev-mode gated** — generating/showing the PSK requires `iot.dev.mode=ON`; ACLs are bypassed in dev-mode so device-ui (httpd) can write+show it. After commissioning, dev-mode OFF locks it (client reads, ds-cli denied). |
| DM-reject recovery | DM **DTLS auth failure OR LwM2M registration 4.0x** → client re-runs bootstrap for fresh DM creds (req #8). |
| Cloud credential store | per-endpoint **array** in a cloud data-store key; BS/DM servers load it **live** (watch + `add_credential`), replacing the shared env-var PSK. Cloud PSK keys mirror the device rule (write-only / no ds-cli read). |
| Client account | **Static system user+group `engineer`** (no-login). `iot-lwm2m-client.service` moves off `DynamicUser=yes` to `User=engineer Group=engineer`. ACLs reference `gid:engineer` (name-based, matches `SO_PEERCRED` primary gid via existing `getgrnam` path). |

## 2. Key facts grounded in the current code

- DTLS PSK callback: `apps/src/dtls_adapter.cpp:129` `dtlsGetPskInfoCb`.
  Identity comes from `inst.identity()`, secret from `inst.get_secret(in)`
  via `device_credentials` (hex strings, decoded by `hexToBinary`).
  BUG-001 (backwards buffer-fit check) — the failure captured in
  `log.txt:238-243` — is already fixed here.
- Credentials installed today via `DTLSAdapter::add_credential(identity,
  secret)` (`apps/inc/dtls_adapter.hpp:156`) from CLI args and from the
  bootstrap commit `apps/src/lwm2m_bootstrap_client.cpp` (~line 236).
- Endpoint: `iot.endpoint` data-store key, hot-reloaded via a watch in
  `apps/src/main.cpp:854-862`; `RegistrationClient::set_endpoint`
  (`apps/inc/lwm2m_registration_client.hpp:21`).
- Data-store ACL: `check_write_acl` enforced in
  `modules/data-store/src/server/worker.cpp:259-267`; **the `Get` handler
  at `worker.cpp:304-333` performs NO ACL check.** `read_acl` is parsed
  (`schema.cpp:204-210`) into `SchemaEntry` but never consulted.
- Peer creds available on session: `session.hpp:58-59`
  `peer_uid()` / `peer_gid()`.
- Schema files: `modules/data-store/schemas/iot.lua` (+ `services.lua`).
- No RPi serial reader exists; analogous `/proc` reader pattern at
  `apps/src/lwm2m_object_3_device.cpp:30-47`.
- Device serial (LwM2M Object 3 RID 2) currently from
  `apps/config/deviceObject/0.lua`.
- ds tests run under podman (`localhost/iot-httpd:lp`); app gtests under
  `apps/test/`.

## 3. New data-store keys

Added to `modules/data-store/schemas/iot.lua`:

| Key | Type | ACL | Notes |
| --- | --- | --- | --- |
| `iot.serial` | string | `write_acl={"gid:engineer"}` | Serial number; populated from RPi or UI. Readable (it is the endpoint). |
| `iot.endpoint` | string | (existing) | Mirror of `iot.serial`; kept for back-compat / explicit override. |
| `iot.dev.mode` | boolean | `write_acl={"gid:engineer"}`, default `false` | Gates PSK read access + CLI write. |
| `iot.bs.psk.identity` | string | `write_acl={"gid:engineer"}`, `read_acl={"gid:engineer"}` | = raw serial; the on-the-wire BS PSK identity. |
| `iot.bs.psk.key` | opaque | `write_acl={"gid:engineer"}`, `read_acl={"gid:engineer"}` | 32-byte PSK, hex-encoded. **Generated by device-ui, written in dev-mode** (ACL bypassed). Write-only / no ds-cli read otherwise. |
| `iot.dm.psk.identity` | string | `write_acl={"gid:engineer"}`, `read_acl={"gid:engineer"}` | = `rpi<serial>@cloud.local`; delivered by bootstrap (req #6), written by the client. |
| `iot.dm.psk.key` | opaque | `write_acl={"gid:engineer"}`, `read_acl={"gid:engineer"}` | Delivered by bootstrap; written by the client (engineer). Write-only / no ds-cli read. |

Two distinct writers:
- **device-ui (httpd)** writes `iot.bs.psk.key` (+ `iot.serial` for non-RPi)
  during commissioning. httpd runs as `iot-httpd` (DynamicUser), *not*
  `engineer`, so it does **not** satisfy `write_acl={"gid:engineer"}`. This
  is fine **because commissioning is dev-mode gated**: with
  `iot.dev.mode=true` the ACLs are bypassed, so the httpd write (and the
  device-ui display of the generated value) succeed. After dev-mode is turned
  off, the keys are locked.
- **the lwm2m client (engineer)** writes `iot.serial`/`iot.endpoint` (RPi
  auto-fill) and the DM creds from bootstrap, and **reads** all PSK keys to
  drive the handshakes — all permitted by `gid:engineer` regardless of
  dev-mode.

The CLI (`ds-cli`), run by an operator as root, matches neither ACL and is
denied — except when `iot.dev.mode == true` (req #7).

Open question for review: confirm `read_acl`/`write_acl` semantics are "deny
non-matching peers, **except** allow all when `iot.dev.mode == true`". See
task C2.

**At-rest note:** the persisted store `data_store.lua` is mode `0600`
(`lua_persistor.cpp:187`) owned by the ds-server's account, and the socket is
`0660` (`server.hpp:30`). read_acl protects the *protocol/CLI* surface only;
a root user can still read the PSK off disk. That matches the requirement
("no read via cli") — root is full-trust. But two deployment items follow:
(1) the `engineer` client must be able to *connect* to the ds socket (shared
group on the socket, or socket group = `engineer`) — see task J;
(2) ds-server's own account must own the persisted file with `0600`.

## 4. TDD work breakdown

Each task: **Red** (write failing test) → **Green** (minimal impl) →
**Refactor**. Tasks are ordered so each builds on green predecessors.
Component tags: [DS]=data-store, [APP]=lwm2m client, [UI]=device-ui,
[CLOUD]=cloud server/ui, [PKG]=packaging.

### A. RPi serial reader [APP]

- **A1 Red:** `apps/test/` gtest `RpiSerialTest`:
  - `ReadsSerialFromDeviceTree` — given a fixture file with
    `100000003d1f9c2e\0`, parser returns `"100000003d1f9c2e"` (NUL/whitespace
    trimmed).
  - `FallsBackToCpuinfo` — device-tree absent, `/proc/cpuinfo` fixture with
    `Serial   : 0000000012345678` → returns `"0000000012345678"`.
  - `ReturnsEmptyWhenNeitherPresent` — both absent → returns `""`
    (caller decides fallback; not the magic `000…0`).
- **A2 Green:** add `read_rpi_serial(devtree_path, cpuinfo_path)` (paths
  injectable for tests; defaults `/proc/device-tree/serial-number`,
  `/proc/cpuinfo`). Mirror style of `read_meminfo`
  (`lwm2m_object_3_device.cpp:30`).
- **A3 Refactor:** expose `is_rpi()` = device-tree serial present and
  non-empty.

### B. PSK generation (C++) [APP/CLOUD]

> Note: the **device's BS PSK** is generated in the **browser** by device-ui
> (task H), not here. This C++ generator is used **cloud-side** to mint the
> **DM PSK** at provisioning (task L) — kept in §4 because it is the same
> primitive and the test is identical.

- **B1 Red:** `PskGenTest`:
  - `Generates32Bytes` — output decodes to exactly 32 bytes.
  - `HexEncodingRoundTrips` — `hexToBinary(generate_psk_hex())` is 32 bytes.
  - `IsNotConstantAcrossCalls` — two calls differ (entropy smoke test).
- **B2 Green:** `generate_psk_hex()` using a CSPRNG
  (`/dev/urandom` or `getrandom(2)`); return 64-char lowercase hex. Reuse
  existing `hexToBinary` for decode.
- **B3 Refactor:** centralise hex helpers if duplicated.

### C. Data-store read_acl enforcement + dev-mode gating [DS]

- **C1 Red (unit):** in data-store server tests, add `SchemaReadAclTest`:
  - `check_read_acl` returns error string for a UID not in `read_acl`.
  - returns `nullopt` (allow) for a listed UID.
  - returns `nullopt` for a key with empty/absent `read_acl`.
- **C2 Red (unit):** `ReadAclDevModeBypass`:
  - When dev-mode is on, `check_read_acl` allows any UID.
    (Decide injection: pass a `bool dev_mode` arg into `check_read_acl`, or
    have the worker consult `iot.dev.mode` and skip the check. Recommend the
    worker reads `iot.dev.mode` once per Get batch and passes the flag —
    keeps SchemaRegistry pure.)
- **C3 Green:** implement `SchemaRegistry::check_read_acl(key, uid, gid)`
  mirroring `check_write_acl` (`schema.cpp:440`). Wire it into the `Get`
  handler (`worker.cpp:304`): per key, if `check_read_acl` denies and
  dev-mode is off, return `proto::Status::SchemaRejected` (or a new
  `AccessDenied`) for that request. Decide: deny whole batch vs per-key
  redaction. **Recommend deny whole Get** containing a protected key (the
  client fetches PSK keys in their own Get, never mixed with config).
- **C4 Red (integration, podman):** ds-tests:
  - `set` PSK key as root succeeds; `get` as non-root → denied.
  - `get` as a process in group `engineer` → allowed (the client must read
    the PSK to drive the handshake); `get` as root (ds-cli) → denied.
  - With `iot.dev.mode=true`, `ds-cli get iot.bs.psk.key` succeeds.
- **C5 Refactor:** ensure watch/notify path also respects read_acl (a
  `watch` on a protected key would leak values via Event). Add
  `WatchRespectsReadAcl` test; gate `RegisterWatch` (`worker.cpp:335`) the
  same way.

> Resolved: read_acl/write_acl name `gid:engineer`; the client runs as
> `engineer` (task J), ds-cli runs as root and is denied. See §3.

### D. Schema keys land [DS]

- **D1 Red:** schema-load test asserting the new keys exist with expected
  types/ACLs (extend existing schema parsing test, or assert via
  `schema_dump`).
- **D2 Green:** add keys from §3 to `schemas/iot.lua`.
- **D3 Refactor:** none expected.

### E. Client wiring: serial → endpoint → identity, PSK from data-store [APP]

- **E1 Red:** `ClientProvisioningTest` (with a fake/mock DsConfig):
  - On RPi (serial reader returns `"ABC123"`), at startup the client writes
    `iot.serial`/`iot.endpoint` = `"ABC123"` (auto-fill) when currently empty;
    client config endpoint == `"ABC123"` and DTLS identity (on the wire) ==
    `"ABC123"` (raw serial).
  - On non-RPi, the client does **not** auto-fill; it uses whatever the
    installer wrote to `iot.serial`. If empty, registration is deferred.
  - The BS PSK key read from `iot.bs.psk.key` is installed into DTLSAdapter
    via `add_credential("ABC123", key)` (keyed by raw serial — what the
    server will see).
  - Precedence: explicit `iot.endpoint`/CLI `ep=` overrides serial when set
    (preserve current behaviour, `main.cpp:626-631`).
- **E2 Green:** extend `DsConfig` (`apps/inc/ds_config.hpp`) with
  `serial()`, `bs_psk_identity()`, `bs_psk_key()`, `dm_psk_identity()`,
  `dm_psk_key()`, `dev_mode()`. In `main.cpp` startup: if `iot.serial` empty
  and `is_rpi()`, populate `iot.serial`/`iot.endpoint`/`iot.bs.psk.identity`
  from `read_rpi_serial()`. **The client does NOT generate the BS PSK** — it
  reads `iot.bs.psk.key` (provisioned by device-ui, task H). Feed
  identity (raw serial) + key into `DTLSAdapter::add_credential`.
- **E3 Refactor:** single provisioning helper
  `provision_bs_credentials(DsConfig&, DTLSAdapter&)`.

### F. Bootstrap DM credential write-back [APP]

- **F1 Red:** extend bootstrap tests: after `apply_commit` with a PSK
  Security instance for the DM server, the client writes
  `iot.dm.psk.identity` and `iot.dm.psk.key` to the data-store (mock
  DsConfig captures the `set`s), write-only keys.
- **F2 Green:** in `lwm2m_bootstrap_client.cpp` `apply_commit`
  (~line 219-239), in addition to `m_dtls->add_credential`, call back into
  a stored `DsConfig` to persist DM identity+key. Inject the writer
  (callback) to keep the bootstrap client decoupled from the data-store.
- **F3 Refactor:** reuse the same persistence callback type as E.

### G. Dev-mode: CLI read/write + self-restart on PSK change [APP/DS]

- **G1 Red (DS):** covered by C2/C4 — CLI can read/write PSK when
  `iot.dev.mode=true`.
- **G2 Red (APP):** `PskChangeRestartTest`:
  - Given a running client (mock), a data-store watch event on
    `iot.bs.psk.key` (or `iot.dm.psk.key`) triggers a clean shutdown path
    (an injected `restart_hook` is invoked exactly once).
  - No restart when dev-mode is off and key is set during initial
    provisioning (avoid restart-on-self-write loop — compare against the
    value we just wrote, or only arm the watch after initial provisioning).
- **G3 Green:** add a watch on the PSK keys in `main.cpp` (alongside the
  existing `iot.endpoint` watch, `main.cpp:854`). On change (post-init),
  log via ACE_DEBUG/ACE_ERROR, flush, and exit non-zero so systemd
  relaunches. Guard against the self-write loop.
- **G4 Refactor:** centralise "exit for restart" so it is testable
  (injected hook in tests, real `ACE_OS::exit` in prod).

### H. device-ui — serial entry + BS PSK generate/display [UI]

- **H1 Red:** Angular specs for the LwM2M config component
  (`iot-ui/src/app/lwm2m/lwm2m-config/`):
  - `serial` form control binds to `iot.serial` via `dbSet`; **read-only**
    when auto-detected (RPi, `iot.serial` already set by the client),
    editable for non-RPi; disabled when not Admin (`session.isAdmin`).
  - A **"Generate BS PSK"** action is visible/enabled **only when
    `iot.dev.mode == true`**. Clicking it produces a 64-hex-char value,
    displays it in a read-only input box (copyable), and writes it via
    `dbSet([{key:'iot.bs.psk.key', ...},{key:'iot.bs.psk.identity', =serial}])`.
  - When `iot.dev.mode == false`, the PSK field and Generate action are
    hidden; no PSK `dbGet` is ever issued.
- **H2 Green:**
  - Add the `serial` field + save wiring (`dbSet([{key:'iot.serial', ...}])`),
    read-only when `iot.serial` is already populated (RPi auto-fill).
  - Implement browser-side PSK generation with
    `crypto.getRandomValues(new Uint8Array(32))` → lowercase hex (keeps the
    secret out of the server until the operator copies it; value shown once,
    then written write-only). Gate behind a `dev_mode` flag read from
    `iot.dev.mode`.
- **H3 Refactor:** extract a `psk-field` component if reused on the cloud UI.

> The httpd `dbSet` of `iot.bs.psk.key` only succeeds because dev-mode
> bypasses `write_acl` (httpd is not `engineer`). See §3.

### I. Packaging — systemd Restart=always [PKG]

- **I1:** ensure `packaging/systemd/iot-lwm2m-client.service` (and the
  mirror under `yocto/meta-iot/recipes-iot/lwm2m/files/`) has
  `Restart=always` (+ sane `RestartSec=`). Add a check/lint or a
  doc-verified manual step (systemd behaviour itself is not unit-tested).

### J. Packaging — static `engineer` account + client de-DynamicUser [PKG]

- **J1 (account creation):** create system user+group `engineer` (no-login
  shell, system range). Use `systemd-sysusers` drop-in
  (`packaging/systemd/` + a `*.conf` line `u engineer - "IoT LwM2M client"`)
  for the systemd target, and a matching `useradd`/`USERADD_PARAM` in the
  Yocto recipe `yocto/meta-iot/recipes-iot/lwm2m/iot_git.bb` for image
  builds. Idempotent.
- **J2 (unit change):** in `iot-lwm2m-client.service` (both copies) replace
  `DynamicUser=yes` with `User=engineer` + `Group=engineer`. Re-evaluate
  the hardening lines that DynamicUser implied (keep `ProtectSystem=strict`,
  `ProtectHome=yes`, `NoNewPrivileges=yes`, `PrivateTmp=yes` as explicit
  directives). Keep any `AmbientCapabilities` the client needs.
- **J3 (socket reachability):** the `engineer` client must connect to the
  ds-server socket (`0660`, `server.hpp:30`). Decide the shared-access
  mechanism: simplest is to make the socket's group `engineer` (or add
  `engineer` to whatever group owns `/var/run/iot`). Verify with an
  integration check that an `engineer`-owned process can `connect()`.
- **J4 (verify):** boot/integration smoke — client comes up as `engineer`,
  reads `iot.bs.psk.key`, ds-cli as root is denied that key, ds-cli as root
  succeeds after `iot.dev.mode=true`. (Overlaps C4 but exercised on the
  real unit/account, not just the daemon.)

> Note: ds-server, httpd, etc. stay on `DynamicUser=yes`; only the lwm2m
> client moves to the static account, because only it needs a stable,
> ACL-matchable primary gid. `getgrnam("engineer")` resolves in ds-server
> regardless of ds-server's own (dynamic) identity, since `engineer` is a
> real `/etc/group` entry.

### K. Re-bootstrap on DM rejection [APP]

- **K1 Red:** `DmRejectRebootstrapTest`:
  - A DM **DTLS handshake failure** (fatal alert / `dtlsGetPskInfoCb`
    failure on the DM session) transitions the client back into the
    bootstrap FSM (`bootstrap::Client` → `AwaitingBSAck`).
  - An LwM2M **registration error** (4.0x, e.g. 4.03) on the DM server
    likewise triggers re-bootstrap.
  - Backoff: at most one re-bootstrap per cooldown window (no tight loop).
- **K2 Green:** wire DM-session DTLS error + `RegistrationClient` 4.0x
  outcome to a `request_rebootstrap()` entry point on the bootstrap client
  (`apps/inc/lwm2m_bootstrap_client.hpp`). Drive it from the client tick
  (`main.cpp` `on_tick_client`, ~416-500). On re-bootstrap success, the new
  DM creds overwrite `iot.dm.psk.*` (task F path).
- **K3 Refactor:** centralise the "fall back to BS" decision so both error
  sources share one guarded path + cooldown.

## 5a. Cloud-side provisioning [CLOUD]

The cloud reuses the same `modules/data-store` daemon and the same
`read_acl` enforcement built in task C — so cloud PSK keys get write-only /
no-ds-cli-read for free, *provided* the cloud server processes have a stable
ACL-matchable identity (mirror of `engineer`; see task P).

### L. Cloud schema + per-endpoint credential array [CLOUD/DS]

- **L1 Red:** schema-load test for new keys in
  `modules/server/lwm2m/schemas/cloud.lua`:
  - `cloud.endpoint.credentials` — string (JSON array), default `"[]"`,
    `read_acl`/`write_acl` = cloud-server group (task P).
  - `cloud.dev.mode` — boolean, default `false` (gates cloud-ui PSK reveal +
    ACL bypass, mirror of `iot.dev.mode`).
- **L2 Green:** add the keys. Array element shape:
  ```jsonc
  { "serial":"<raw>", "identity":"rpi<raw>@cloud.local",
    "bs_psk_key":"<hex>", "dm_psk_id":"rpi<raw>@cloud.local",
    "dm_psk_key":"<hex>" }
  ```
- **L3 Refactor:** none.

### M. Cloud provision backend — store BS PSK + mint DM PSK [CLOUD]

Provisioning is **db/set-driven** (no bespoke REST endpoint — see resolved
item #8). cloud-ui writes `cloud.provision.bs_psk` (carrier) + sets
`cloud.provision.request` = serial (existing watched trigger).

- **M-core (DONE):** `cloud_credentials.{hpp,cpp}` — `format_identity`,
  `upsert_credential` (idempotent on serial), `remove_credential`; 7 tests.
- **M-wire Red:** iot-cloudd provision-handler test: on a
  `cloud.provision.request`=serial event with `cloud.provision.bs_psk` set,
  the handler mints a DM PSK (`generate_psk_hex`), calls `upsert_credential`,
  writes `cloud.endpoint.credentials`, and clears the carrier. Reject blank
  serial / BS PSK not 64-hex.
- **M-wire Green:** in `apps/cloud/server/src/main.cpp` provision watch
  (~280-298): read `cloud.provision.request` (serial) + `cloud.provision.bs_psk`;
  `id = format_identity(serial)`; `dm = generate_psk_hex()`; read-modify-write
  `cloud.endpoint.credentials` via `upsert_credential`; `ds.set` carrier="".
  (BS PSK is device-generated; cloud mints ONLY the DM PSK.) Add schema key
  `cloud.provision.bs_psk` (string, `gid:cloud-svc` ACLs) to `cloud.lua`.
- **M-wire Refactor:** share `format_identity` with the servers (task N).

### N. BS/DM servers — live per-endpoint credentials [CLOUD]

- **N1 Red:** `ServerCredentialLoaderTest`:
  - Given a `cloud.endpoint.credentials` array, the loader registers, for the
    **BS** server, `add_credential(serial, bs_psk_key)` (keyed by **raw
    serial** — what arrives on the wire) and, for the **DM** server,
    `add_credential("rpi<serial>@cloud.local", dm_psk_key)`.
  - A watch event with an added/removed element updates the DTLSAdapter
    credential map live (no restart).
  - BS bootstrap response pushes the DM security object (DM URI, DM
    identity = formatted, DM key) into the device (extends
    `build_security_tlv`, `bootstrap.cpp:84-97`, which currently omits PSK
    id/key).
- **N2 Green:** in the cloud LwM2M server main (`apps/cloud/server` /
  `apps/src/lwm2m_bootstrap_server.cpp`), replace the env-var PSK
  (`docker-compose.yml` `LWM2M_PSK_ID/KEY`) with: on startup + on
  `cloud.endpoint.credentials` watch, parse the array and populate the BS and
  DM DTLSAdapters via `add_credential`. Build the DM security-object TLV from
  the matching array element during bootstrap.
- **N3 Refactor:** one `apply_credentials(array, bsAdapter, dmAdapter)` used
  by both the initial load and the watch callback.

> **Implemented (drift from plan):** the env-var PSK (`LWM2M_PSK_ID/KEY`) was
> *removed entirely* — no hardcoded/shared default. Instead of a startup load +
> watch populating `add_credential`, the server installs a **ds-backed PSK
> resolver** (`DTLSAdapter::set_psk_resolver`, wired in `apps/src/main.cpp`)
> that looks the key up *live* from `cloud.endpoint.credentials` for the
> identity presented at each handshake (BS `sha256(serial)[:32]`, DM
> `dm.psk.id`). This avoids a standing watch + map upkeep and is safe because
> the callback runs on the handshake thread, not the ds listener thread.

### O. Cloud-ui — Endpoints tab + provision form [CLOUD/UI]

- **O1 Red:** Angular specs (`apps/cloud/ui/src/app/...`):
  - A new **Endpoints** tab lists provisioned endpoints (serial, formatted
    identity, BS/DM PSK *status* only — never the secret in prod).
  - A provision form collects **serial** + **BS PSK** (pasted from device-ui)
    and provisions via `db/set` (`cloud.provision.bs_psk` + `cloud.provision.request`
    = serial) — NOT a bespoke endpoint; client-side validates BS PSK = 64 hex.
  - DM PSK is server-minted; the form does not collect it. PSK values are
    revealed only when `cloud.dev.mode == true`.
- **O2 Green:** add the tab/route + form using the existing `dbSet`; render
  the credentials list (status fields only) from `cloud.endpoint.credentials`.
- **O3 Refactor:** reuse the `psk-field` component from task H if practical.

### P. Cloud packaging — server account + ACL identity [PKG]

- **P1:** give the cloud LwM2M server (BS/DM) processes a stable
  ACL-matchable identity so `read_acl`/`write_acl` on `cloud.endpoint.*` can
  name them and `ds-cli` (root) is denied — mirror of task J on the cloud
  side. In the containerised cloud (`docker-compose.yml`) decide: a static
  `cloud-svc` user/group the server containers run as, with the ds socket
  group-shared. Document the equivalent for any non-container cloud deploy.
- **P2 (verify):** integration — provision via cloud-ui, confirm a device
  with that serial completes the BS handshake, receives DM creds, and
  registers with the DM server; `ds-cli` on the cloud is denied the PSK keys
  unless `cloud.dev.mode=true`.

## 5. Suggested merge order (small PRs)

**Device side**
1. J (static `engineer` account + unit + socket access) — foundational;
   the ACL identity everything else relies on.
2. A + B (pure helpers, no wiring) — fast, isolated.
3. D + C (schema keys + read_acl enforcement; the security core).
4. E (serial → endpoint/identity; read BS PSK from ds).
5. F (DM write-back) + K (re-bootstrap on DM reject).
6. G (dev-mode restart) + I (Restart=always).
7. H (device-ui: serial + dev-mode BS-PSK generate/display).

**Cloud side** (can proceed in parallel after C/D land the ACL mechanism)
8. P (cloud server account + ACL identity) — foundational, mirror of J.
9. L (cloud schema + credential array).
10. M (provision backend: store BS PSK, mint DM PSK).
11. N (BS/DM servers load creds live + push DM security object).
12. O (cloud-ui Endpoints tab + provision form).

End-to-end integration (P2) closes it out.

## 6. Open items to confirm before coding

1. ~~**Client runtime UID**~~ — RESOLVED: static `engineer` user+group;
   ACLs by `gid:engineer`. See decisions table, §3, task J.
2. **read_acl on dev-mode** — bypass-when-on semantics (C2) vs separate
   allow-list. Recommended: worker passes `dev_mode` flag, check bypassed.
3. **Get-deny granularity** — whole-batch deny vs per-key redaction (C3).
   Recommended whole-batch.
4. **Self-write loop guard** strategy (G3): suppress watch during init vs
   value-compare. Recommended: arm PSK watch only after provisioning done.
5. Whether `iot.endpoint` stays separate from `iot.serial` or is fully
   replaced.
6. **Socket-access mechanism** for the `engineer` client (J3): socket group
   = `engineer` vs a shared `iot` group. Pick during J.
7. ~~**Cloud server account**~~ — RESOLVED: static `cloud-svc` user/group;
   the BS/DM containers run as it, ds socket group-shared (task P).
8. ~~**Provision API shape**~~ — RESOLVED (revised): **no bespoke REST
   endpoint**. `POST /api/v1/cloud/endpoints` is itself just
   `ds.set("cloud.provision.request", ep)` (handler.cpp:755), so cloud-ui
   provisions purely via `db/set`: a `cloud.provision.bs_psk` carrier key +
   the existing `cloud.provision.request` (= serial) trigger. iot-cloudd's
   watch formats the identity, mints the DM PSK, and upserts the credential
   array. BS PSK is device-generated; the cloud generates ONLY the DM PSK.
   Cloud provisioning runs under `cloud.dev.mode` (ACL bypass for the
   carrier write); iot-cloudd clears the carrier after upsert.

### Resolved this round

- PSK generator: **device-ui** (browser) for BS PSK; **cloud** mints DM PSK.
- device-ui PSK reveal: **dev-mode gated**.
- BS DTLS identity on the wire: **raw serial**; `ep=` raw serial.
- Cloud canonical identity / DM identity: **`rpi<serial>@cloud.local`** for
  all hardware (literal `rpi` prefix).
- RPi serial field in device-ui: **read-only** (auto-detected); installer
  entry for non-RPi.
- Cloud credential store: **per-endpoint JSON array** loaded live by BS/DM
  servers; cloud PSK keys mirror device write-only rule.
- DM-reject recovery: **DTLS failure OR registration 4.0x** → re-bootstrap.
- OpenVPN tunnel: **out of scope** for the LwM2M path (device-ui proxy only).
