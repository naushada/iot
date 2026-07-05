# TDD: Device-side Dynamic DNS (`iot-ddnsd`)

Status: **IN PROGRESS** (2026-07-05) — schema + daemon skeleton landing first.

An on-device ACE-reactor daemon that keeps a public DNS **A** record pointed at
the device's current public IPv4, across four pluggable provider backends. Ships
**disabled by default**.

Companion docs: `apps/docs/ddns-plan.md` (plan + provider survey),
`apps/docs/prd-ddns.md` (PRD). Epic: **#512**.

## Implementation status

- [ ] PR-1 (#513, #514) — `ddns.lua` schema + `modules/ddns/` + `iot-ddnsd`
      skeleton (reactor, ds watch, timer, state publish). [DEVICE]
- [ ] PR-2 (#515) — `PublicIpDetector` (HTTPS echo + fallbacks). [DEVICE]
- [ ] PR-3 (#516, #517) — `ProviderBackend` iface + **dyndns2** + **DuckDNS**;
      first live update. [DEVICE]
- [ ] PR-4 (#520, #521) — systemd/yocto packaging + secrets handling. [PKG]
- [ ] PR-5 (#518) — **Cloudflare** backend. [DEVICE]
- [ ] PR-6 (#519) — **Route53** backend (SigV4). [DEVICE]
- [ ] PR-7 (#522) — device-ui DDNS page. [DEVICE/UI]
- [ ] PR-8 (#523) — reachability flag (CGNAT awareness). [DEVICE]
- [ ] HW validation on RPi3B (Wi-Fi/home-NAT) + mangOH (cellular).

## 1. Goal

Give each device a stable hostname (operator-chosen, e.g.
`dev-<serial>.example.com`) that automatically tracks its current public IPv4, so
the device is directly reachable (SSH/device-ui/diagnostics) even as its WAN IP
churns behind NAT/CGNAT — without any manual step after configuration.

### v1 scope decisions

- **Device-side only.** Cloud/Vultr DDNS is a separate initiative (`cloud.dm.uri`
  is the insertion point there — see plan doc).
- **IPv4 / A record only.** AAAA is a fast-follow.
- **Update, not create.** Operator pre-creates the hostname/zone; we only UPSERT.
- **Single hostname per device.**
- **Four backends:** dyndns2 (generic), DuckDNS, Cloudflare, Route53.
- **Detection = HTTPS echo** with fallbacks (device can't see its own public IP);
  DuckDNS/dyndns2 may use server-side autodetect to save a round-trip.

## 2. Locked design decisions

1. **One ACE reactor daemon**, `iot-ddnsd`, in a new module `modules/ddns/`,
   mirroring `modules/wan/cellular/daemon` (`class DdnsClient : public
   ACE_Event_Handler`, `handle_timeout` for the poll cadence, `handle_exception`
   as the reactor wakeup from the ds-watch listener thread). No raw POSIX.
2. **Config + state live in ds** under a new `ddns.*` namespace (`ddns.lua`).
   Secrets use the **write-only** access class; the daemon reads them, viewers
   can't.
3. **Update is idempotent**: call the provider only when the detected IP differs
   from `ddns.last.ip`, or a **forced-refresh window** (default 24h) elapsed.
4. **Provider backends** hide behind a `ProviderBackend` strategy interface so
   the daemon core is provider-agnostic; adding a provider = one new class.
5. **HTTPS client** is a small ACE-reactor-integrated TLS client (reuse the TLS
   stack already linked; OpenSSL is present for the cloud/cert paths). All
   sockets registered on the daemon's reactor — no blocking curl.
6. **Backoff** on error (exponential + jitter), capped; never hammer a provider.
7. **Secrets on disk** follow the VPN-cert/BS-KEK pattern: optional
   `/etc/iot/ddns/<provider>.token` (`0640`) referenced by a `*.path` ds key or
   systemd `LoadCredential=`, as an alternative to the write-only ds key set from
   device-ui.
8. **Starts after time is sane** — HTTPS needs a valid clock (NTP-no-RTC window);
   the daemon retries rather than crash-looping on "cert not yet valid".

## 3. Components (`modules/ddns/`)

```
modules/ddns/
  inc/ddns/
    provider.hpp        # ProviderBackend interface + Result/Creds structs
    public_ip.hpp       # PublicIpDetector interface
    ddns_state.hpp      # config + runtime state structs, State enum
  src/
    provider_dyndns2.cpp
    provider_duckdns.cpp
    provider_cloudflare.cpp
    provider_route53.cpp
    public_ip.cpp       # echo detector w/ fallbacks
    sigv4.cpp           # AWS SigV4 signer (Route53)
    http_client.cpp     # minimal ACE + TLS request helper
  daemon/
    main.cpp            # arg parse → DdnsClient::run()
    ddns_client.cpp/.hpp
  test/                 # gtest: parsers, sigv4 vectors, detector stub
  CMakeLists.txt        # gated by DDNS_BUILD_DAEMON (default OFF)
```

Pure logic (`src/`, `inc/`) is unit-testable without ds/network; the daemon
(`daemon/`) wires it to the reactor + ds. Same split as sensors/cellular.

## 4. Data-store keys (`ddns.lua`)

```lua
-- Config (operator-set)
ddns.enabled          bool   default false            (Admin)
ddns.provider         string dyndns2|duckdns|cloudflare|route53  (Admin)
ddns.hostname         string FQDN to keep updated     (Admin)
ddns.interval         int    default 300  min 30 max 86400  (Admin)  -- poll cadence (s)
ddns.refresh.force    int    default 86400              (Admin)  -- forced re-push window (s)
ddns.ip.source        string echo|dyndns2-auto|cloud  (Admin, default echo)

-- Provider targets (non-secret)
ddns.dyndns2.server   string default members.dyndns.org  (Admin)
ddns.dyndns2.user     string                              (Admin)
ddns.duckdns.domains  string                              (Admin)   -- subdomain label(s)
ddns.cf.zone.id       string                              (Admin)
ddns.cf.record.name   string                              (Admin)
ddns.r53.zone.id      string                              (Admin)
ddns.r53.record.name  string                              (Admin)
ddns.r53.access.key   string                              (Admin)

-- Secrets (WRITE-ONLY access class; never returned on read)
ddns.dyndns2.token    string  (WriteOnly)
ddns.duckdns.token    string  (WriteOnly)
ddns.cf.token         string  (WriteOnly)
ddns.r53.secret.key   string  (WriteOnly)
ddns.token.path       string  (Admin)   -- optional file override for the active provider's secret

-- Runtime state (daemon → device-ui; read-only to viewers)
ddns.state            string disabled|waiting-clock|detecting|updating|ok|ok-unreachable|error
ddns.last.ip          string last published public IPv4
ddns.last.ok.ts       int    epoch of last successful update
ddns.last.error       string last provider/echo error (human readable, no secrets)
ddns.version          int    bump-on-change for the device-ui long-poll
```

Access classes reuse the `Admin` / `Viewer` / write-only conventions already in
the schemas (`admin_str`, etc.). Verify the exact write-only helper name in the
schema lib before coding (PSK provisioning already introduced one).

## 5. `iot-ddnsd` internals

**Lifecycle** (`DdnsClient::run()`):
1. `m_ds.connect(cfg.ds_sock)` — non-zero exit on failure (systemd restarts).
2. `load_config_from_ds()` — bulk `get` all `ddns.*` config + secrets into
   `m_cfg`/`m_creds`.
3. If `!ddns.enabled` → publish `state=disabled`, still `watch` config so a later
   enable wakes us.
4. `watch({ddns.enabled, ddns.provider, ddns.hostname, ddns.interval,
   ddns.ip.source, net.iface.active.ip, ...}, cb, &h)` — the callback runs on the
   ds listener thread, records a dirty flag under `m_mtx`, and `reactor()->
   notify(this)` → `handle_exception` reloads on the reactor thread.
5. Schedule `handle_timeout` every `ddns.interval`.
6. `reactor()->run_reactor_event_loop()`.

**Tick** (`handle_timeout`): if enabled →
`state=detecting` → `PublicIpDetector::detect()` → if IP changed OR
force-refresh elapsed → `state=updating` → `backend->update(hostname, ip,
creds)` → on ok publish `last.ip`/`last.ok.ts`/`state=ok` (+ optional
reachability probe → `ok-unreachable`); on error `last.error`/`state=error` +
schedule a backoff one-shot timer.

**Config reload** (`handle_exception`): re-read dirty `ddns.*` keys, rebuild the
active `ProviderBackend` if `provider` changed, re-arm the timer if `interval`
changed, and if `net.iface.active.ip` changed force an immediate detect (react to
reconnects without waiting a full interval).

**`ProviderBackend` interface**
```cpp
struct Creds { std::string user, secret, extra1, extra2; };   // provider-mapped
struct Result { bool ok; int code; std::string msg; };        // msg has no secret
struct ProviderBackend {
  virtual ~ProviderBackend() = default;
  virtual Result update(const std::string& host, const std::string& ip,
                        const Creds&) = 0;
};
```
`make_backend(provider)` returns the right impl. dyndns2/duckdns build a URL +
GET; cloudflare does list→PUT with a cached record id; route53 signs SigV4 +
POSTs XML.

## 6. Yocto / image / packaging prerequisites

Two unit copies kept in sync (per project convention):
- `packaging/systemd/iot-ddnsd.service` + `packaging/etc-iot/ddnsd.env`
  (`IOT_DDNS_ARGS`, e.g. `--ds-sock /run/iot/data_store.sock`).
- `yocto/meta-iot/recipes-iot/lwm2m/files/iot-ddnsd.service`.

Unit body (mirror `iot-sensord.service`): `After=network-online.target
iot-ds.service`, `Wants=iot-ds.service`, `Restart=on-failure`, journald,
hardening (`NoNewPrivileges`, `PrivateTmp`, `ProtectKernelTunables`, …),
`[Install] WantedBy=multi-user.target`. **No `RuntimeDirectory=iot`.** For a
file-based secret add `LoadCredential=ddns_secret:/etc/iot/ddns/token`.

Recipe (`yocto/.../lwm2m/iot_git.bb`):
- `SRC_URI += file://iot-ddnsd.service`
- `PACKAGE_BEFORE_PN += ${PN}-ddns`
- `FILES:${PN}-ddns = "${bindir}/iot-ddnsd ${systemd_system_unitdir}/iot-ddnsd.service"`
- `RDEPENDS:${PN}-ddns = "ace-tao"`, `RRECOMMENDS:${PN}-ddns += "${PN}-ds-server ${PN}-config"`
- `SYSTEMD_SERVICE:${PN}-ddns = "iot-ddnsd.service"`,
  `SYSTEMD_AUTO_ENABLE:${PN}-ddns = "enable"`

**Preset gotcha:** add `enable iot-ddnsd.service` to
`yocto/.../lwm2m/files/90-iot.preset` (else first-boot `preset-all` disables it).
Add `iot-ddns` to `packagegroup-iot.bb`.

CMake: `modules/ddns/CMakeLists.txt` gated by `DDNS_BUILD_DAEMON` (default OFF,
links `datastore_client ACE pthread` + the TLS lib); enable + `add_subdirectory`
from `apps/CMakeLists.txt`, and add the unit/env to its `install(FILES ...)`
blocks.

## 7. device-ui page

Angular DDNS page (Admin-gated), Clarity conventions (`clr-datagrid` for lists,
4-column `.form-grid` forms, pad short rows with empty cells):
- Provider `<select>`, hostname, interval, ip.source.
- Provider-specific credential fields (write-only inputs; never populated from a
  GET — show "•••• set" placeholders).
- `enabled` toggle.
- Status tile: `state`, `last.ip`, `last.ok.ts` (relative), `last.error`.

## 8. Phasing (merge order)

PR-1 schema+skeleton → PR-2 detector → PR-3 dyndns2+DuckDNS (first live update,
DuckDNS smoke) → PR-4 packaging+secrets → PR-5 Cloudflare → PR-6 Route53 →
PR-7 device-ui → PR-8 reachability flag → HW validation. Each PR small and
self-contained; the daemon is usable (logs detected IP) from PR-2 on.

## 9. Testing

- **Unit (gtest, `test/`)**: dyndns2/DuckDNS/Cloudflare response parsing;
  SigV4 against a published AWS test vector; `PublicIpDetector` with a stub echo
  (success, fallback, all-fail); idempotence (no update when IP unchanged);
  backoff schedule.
- **Integration (podman)**: run `iot-ddnsd` against a local ds; drive config via
  `DS set`; point a backend at a local mock HTTP server; assert `ddns.*` state.
- **HW e2e**: RPi3B on Wi-Fi/home-NAT with DuckDNS + dyndns2 — external `dig`
  resolves to the new IP after a forced IP change; mangOH cellular exercises the
  CGNAT `ok-unreachable` path.

## 10. Security notes

- Secrets never committed, never logged, never returned on ds read / device-ui
  GET. Write-only ds keys or `0640` credential files only.
- Recommend scoped tokens: Cloudflare Zone:DNS:Edit on one zone; Route53
  least-priv (`route53:ChangeResourceRecordSets` on the one hosted zone).
- TLS verify on all provider/echo calls; no plaintext HTTP.
- `ddns.last.error` sanitized (strip any echoed credential).

## 11. Open items

- Forced-refresh default (24h?) — Q1 in PRD.
- Cloud-fed IP source (`iot.wan.ip`) vs echo — Q3; echo for v1.
- Hostname naming convention — Q4; operator-free-form.
- Reachability probe mechanism (who probes from outside? a cloud assist, or a
  known-open port check?) — refine in PR-8.

## 12. Hardware test recipe

```sh
# On the device (RPi3B), once iot-ddnsd is installed + enabled:
DS set ddns.provider '"duckdns"'
DS set ddns.hostname '"mydev.duckdns.org"'
DS set ddns.duckdns.domains '"mydev"'
DS set ddns.duckdns.token '"<uuid>"'      # write-only
DS set ddns.enabled true
systemctl restart iot-ddnsd
journalctl -u iot-ddnsd -f                # watch detect → update → ok
DS get ddns.state ; DS get ddns.last.ip
# From a machine off-net:
dig +short mydev.duckdns.org              # should equal ddns.last.ip
```
