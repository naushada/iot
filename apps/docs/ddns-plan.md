# DDNS (Dynamic DNS) — Feature Plan & Provider Survey

Status: **Planning** · Owner: naushada · Created 2026-07-05

This document chalks out the plan for a **device-side** Dynamic DNS feature: an
on-device daemon (`iot-ddnsd`) that keeps a public DNS record pointed at the
device's current public IP address, across the four provider backends selected
for v1.

Companion docs:
- `apps/docs/prd-ddns.md` — product requirements (the "what/why")
- `apps/docs/tdd-ddns.md` — technical design (the "how", PR roadmap)

---

## 1. Why device-side DDNS

Devices (RPi3B, mangOH Yellow) live behind home NAT (Wi-Fi) or carrier CGNAT
(cellular). Their WAN-facing public IP churns on lease renewal, reconnect, or
tower handoff. Today a device is reachable from the outside **only** through the
OpenVPN tunnel + cloud reverse proxy. DDNS adds a second, direct addressing
path: a stable per-device hostname (e.g. `dev-<serial>.example.com`) that always
resolves to the device's *current* public IP.

Use cases this unlocks:
- Direct SSH / device-ui reach without the VPN+proxy hop (when the WAN IP is
  publicly routable — i.e. real home broadband, not CGNAT).
- A stable name for field diagnostics and third-party integrations.
- Foundation for future inbound services (webhooks, direct MQTT) terminating on
  the device.

### The hard constraint: the device does not know its own public IP

Confirmed during code exploration — the device only ever sees **private/LAN**
addresses:
- `cell.ip` — IP on the cellular wwan0 data context (often CGNAT/private)
- `net.iface.active.ip` — routable LAN IPv4 chosen by net-router (eth→wifi→cell)
- `wifi.dhcp.ip` / `wifi.dhcp.gateway` — DHCP lease on the LAN
- `net.tun.ip` — the VPN-assigned tunnel IP

The device's true public IP is observed **only cloud-side**, from the DTLS/VPN
peer address (`apps/cloud/server/src/main.cpp` `ep.wan_ip`). Therefore the DDNS
daemon must **discover** the public IP, via one of:

1. **External IP-echo** (primary): HTTPS GET to an echo endpoint
   (`https://api.ipify.org`, `https://checkip.amazonaws.com`, `icanhazip.com`),
   parse the returned IP. Simple, provider-agnostic, works everywhere with
   egress. Must be pluggable + have >1 fallback endpoint.
2. **Cloud-fed** (secondary/optional): the cloud already knows each device's
   `wan_ip`; it could publish it back over the existing LwM2M/ds channel
   (a new `iot.wan.ip` key). Avoids a third-party echo dependency but couples
   DDNS to a live cloud link. Considered for a later phase.
3. **dyndns2 auto-detect**: some providers (DuckDNS with blank `ip=`, dyndns2
   with `myip` omitted) infer the source IP of the update request. Zero echo
   needed, but only works when the update egresses from the same public IP we
   want published (true for NAT, **not** for split-tunnel/proxied egress).

v1 uses strategy **1** (echo with fallbacks) as the portable default, and
opportunistically strategy **3** for DuckDNS/dyndns2 to save a round-trip.

---

## 2. Architecture at a glance

```
                         ┌────────────────────────────────────────┐
                         │              iot-ddnsd                  │
                         │  (ACE_Reactor single-thread daemon)     │
                         │                                         │
  data-store  ──watch──▶ │  DsBridge: ddns.enabled/provider/       │
  (ddns.*)               │           hostname/interval/creds       │
                         │                │                        │
                         │                ▼                        │
   WAN up  ──net.iface──▶ │   PublicIpDetector ──HTTPS──▶ ipify/    │
   change     .active.ip  │     (echo + fallbacks)        checkip   │
                         │                │                        │
                         │                ▼ (ip changed?)          │
                         │   ProviderBackend (strategy iface)       │
                         │    ├─ Dyndns2Backend   (HTTP GET)        │
                         │    ├─ CloudflareBackend (REST/token)     │
                         │    ├─ Route53Backend    (SigV4 UPSERT)   │
                         │    └─ DuckDnsBackend     (HTTP GET)      │
                         │                │                        │
                         │                ▼                        │
  data-store  ◀─write──  │  publish ddns.state/last.ip/last.ok/    │
  (ddns.state...)        │          last.error/last.update.ts      │
                         └────────────────────────────────────────┘
```

- **Single ACE-reactor daemon**, no raw POSIX (project rule). All I/O — the ds
  socket fd, the periodic timer, and the HTTPS client — is driven off one
  `ACE_Reactor`, matching the openvpn/net-router DsBridge daemons.
- **Trigger model**: update when (a) the periodic timer fires
  (`ddns.interval`, default 300s), OR (b) `net.iface.active.ip` /
  connectivity changes (event-driven, so we react to reconnects immediately),
  OR (c) config changes (provider/hostname/creds edited in device-ui).
- **Idempotent**: only call the provider when the detected public IP differs
  from `ddns.last.ip` (or a forced-refresh interval elapses, to defend against
  out-of-band record edits and free-tier "confirm every N days" expiry).
- **Backoff**: on provider error, exponential backoff with jitter; never hammer
  a provider (some free tiers ban abuse).

---

## 3. Provider survey (v1 backends)

All four selected. Common surface: given `(hostname, ip, credentials)`, make the
record resolve to `ip`. They differ in transport, auth, and how they identify
the record.

### 3.1 dyndns2 (generic) — the lingua franca

The de-facto standard defined by Dyn in the late 90s; spoken by No-IP, Dynu,
deSEC, IPv64, DynIP, most routers, ddclient, inadyn.

- **Request**: `GET https://<server>/nic/update?hostname=<host>&myip=<ip>`
- **Auth**: HTTP Basic (`user:token`). Token, never account password.
- **Response** (text): `good <ip>` / `nochg <ip>` (success), or errors
  `nohost`, `badauth`, `notfqdn`, `abuse`, `911`, `!donator`.
- **IP autodetect**: omit `myip` → server uses request source IP.
- **Config we need**: `server` (host), `username`, `password/token`, `hostname`.
- **Why**: one code path covers a large fraction of providers; ideal generic
  fallback. **This is the backbone backend** — implement first.

### 3.2 DuckDNS — zero-friction, churn-tolerant

Free, AWS-hosted, tolerant of rapid IP changes (good for LTE/5G). No real domain
needed — great for bring-up/testing.

- **Request**: `GET https://www.duckdns.org/update?domains=<sub>&token=<token>&ip=<ip>`
  (blank `ip=` → autodetect; supports `&ipv6=` too).
- **Auth**: `token` query param (UUID).
- **Response** (text): `OK` / `KO`.
- **Config we need**: `domains` (subdomain label), `token`.
- **Why**: simplest possible backend; use it as the first end-to-end smoke test
  target before wiring the heavier APIs.

### 3.3 Cloudflare API — token-scoped, robust

For when you own a real zone on Cloudflare. Scoped API tokens limit blast radius
to a single zone/record.

- **Discover record id**:
  `GET /client/v4/zones/<zone_id>/dns_records?type=A&name=<host>`
- **Update**:
  `PUT|PATCH /client/v4/zones/<zone_id>/dns_records/<record_id>`
  body `{"type":"A","name":"<host>","content":"<ip>","ttl":60,"proxied":false}`
- **Auth**: `Authorization: Bearer <api_token>` (scoped: Zone:DNS:Edit).
- **Response**: JSON `{"success":true,...}`.
- **Config we need**: `api_token`, `zone_id`, `record_name` (record id cached
  after first lookup, re-resolved on 404).
- **Why**: the production-grade path when the fleet has a controlled domain.
  Needs a JSON parse + record-id cache.

### 3.4 Route53 (AWS) — heaviest, SigV4

Fits if the domain is hosted in AWS. The costly one: requests must be **AWS
SigV4-signed** (HMAC-SHA256 canonical-request chain).

- **Update**: `POST /2013-04-01/hostedzone/<zone_id>/rrset` with a
  `ChangeResourceRecordSets` XML body, `Action=UPSERT`, `Type=A`, `TTL=60`.
- **Auth**: SigV4 over `route53.amazonaws.com` (service `route53`,
  region `us-east-1`), from `access_key_id` + `secret_access_key`.
- **Response**: XML `<ChangeInfo>`; poll `GetChange` optional.
- **Config we need**: `access_key_id`, `secret_access_key`, `hosted_zone_id`,
  `record_name`.
- **Complexity**: implement SigV4 on top of the crypto already linked for TLS
  (OpenSSL HMAC-SHA256). Ship this backend **last**; guard behind the same
  `ProviderBackend` interface so the daemon is agnostic.

### Backend comparison

| Backend    | Transport        | Auth               | Record id | IP autodetect | Impl cost | Order |
|------------|------------------|--------------------|-----------|---------------|-----------|-------|
| dyndns2    | HTTP GET         | Basic user:token   | hostname  | yes (omit ip) | low       | 1     |
| DuckDNS    | HTTP GET         | token param        | domains   | yes (blank ip)| very low  | 2     |
| Cloudflare | REST JSON PUT    | Bearer token       | lookup+id | no            | medium    | 3     |
| Route53    | REST XML POST    | AWS SigV4          | zone+name | no            | high      | 4     |

---

## 4. Config & secrets model (ds keys, sketch)

New namespace `ddns.*` (schema `modules/.../schemas/ddns.lua`). Final key list is
fixed in the TDD; sketch:

```
ddns.enabled          bool    default false        (Admin)
ddns.provider         string  dyndns2|duckdns|cloudflare|route53
ddns.hostname         string  fqdn to keep updated
ddns.interval         int     seconds, default 300
ddns.ip.source        string  echo|dyndns2-auto|cloud   (detection strategy)

# secrets — write-only access class, never returned to viewers
ddns.dyndns2.server   string
ddns.dyndns2.user     string
ddns.dyndns2.token    string  (write-only)
ddns.duckdns.domains  string
ddns.duckdns.token    string  (write-only)
ddns.cf.zone.id       string
ddns.cf.record.name   string
ddns.cf.token         string  (write-only)
ddns.r53.zone.id      string
ddns.r53.record.name  string
ddns.r53.access.key   string
ddns.r53.secret.key   string  (write-only)

# runtime state — read-only, for device-ui
ddns.state            string  disabled|detecting|updating|ok|error
ddns.last.ip          string  last published public IP
ddns.last.ok.ts       int     epoch of last successful update
ddns.last.error       string  last provider error (human readable)
```

Secrets follow the existing write-only pattern (like the PSK / VPN creds):
settable by Admin, never echoed back over the ds read path or the device-ui API.

---

## 5. Phasing (detailed roadmap lives in the TDD)

1. **P1 — Schema + daemon skeleton**: `ddns.lua`, `iot-ddnsd` ACE reactor,
   DsBridge watch, systemd unit + preset + tmpfiles. No provider yet — logs the
   detected IP.
2. **P2 — PublicIpDetector**: HTTPS echo with fallbacks; publish
   `ddns.last.ip`.
3. **P3 — dyndns2 + DuckDNS backends**: first real end-to-end update
   (DuckDNS smoke test).
4. **P4 — Cloudflare backend**: JSON + record-id cache.
5. **P5 — Route53 backend**: SigV4 signer.
6. **P6 — device-ui**: DDNS config page (Clarity clr-datagrid/form-grid) +
   status tile; write-only secret fields.
7. **P7 — HW validation** on RPi3B (Wi-Fi/home-NAT) and mangOH (cellular),
   including the CGNAT "published but unreachable" caveat documented.

## 6. Risks / open questions

- **CGNAT**: on carrier cellular the published IP is often un-routable. DDNS
  still succeeds but the name is unreachable. Document clearly; consider a
  reachability probe that flags `ddns.state=ok-unreachable`.
- **IPv6**: v1 is A-record/IPv4 only; AAAA is a fast-follow (dyndns2 `myipv6`,
  DuckDNS `ipv6=`, Cloudflare AAAA, Route53 AAAA).
- **Route53 SigV4** in C++ is the biggest single cost; keep it isolated so it
  can slip a phase without blocking the rest.
- **Echo dependency**: third-party echo endpoints can rate-limit; ship ≥2
  fallbacks and a long cache.
- **Clock skew / TLS**: NTP-no-RTC issue (see memory) means HTTPS to providers
  can fail with "cert not yet valid" pre-sync — DDNS must start *after* time is
  sane, or tolerate/retry.

---

## Provider sources

- [6 Best Dynamic DNS Providers 2026 — Comparitech](https://www.comparitech.com/net-admin/dynamic-dns-providers/)
- [dyndns2 update API — Dyn Help Center](https://help.dyn.com/remote-access-api.html)
- [No-IP integrate / update API](https://www.noip.com/integrate/request)
- [DuckDNS](https://www.duckdns.org/)
- [Cloudflare — managing dynamic IP addresses](https://developers.cloudflare.com/dns/manage-dns-records/how-to/managing-dynamic-ip-addresses/)
- [deSEC dyndns update API](https://desec.readthedocs.io/en/latest/dyndns/update-api.html)
