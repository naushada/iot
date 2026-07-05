# PRD — Device-Side Dynamic DNS (DDNS)

Status: **Draft** · Owner: naushada · Created 2026-07-05

Related: `apps/docs/ddns-plan.md` (plan + provider survey) ·
`apps/docs/tdd-ddns.md` (technical design)

---

## 1. Problem

Field devices (RPi3B, mangOH Yellow) sit behind home NAT or carrier CGNAT. Their
WAN public IP changes on lease renewal, reconnect, or tower handoff. Today a
device is reachable from outside **only** via the OpenVPN tunnel + cloud reverse
proxy. There is no stable, direct name that resolves to a device's current
public IP, and the device cannot even observe its own public IP (it sees only
private/LAN addresses). Operators and integrations that want direct reach
(SSH, device-ui, diagnostics) have no addressable target that survives an IP
change.

## 2. Goals

- G1 — Give each device a **stable DNS hostname** that automatically tracks its
  current public IPv4.
- G2 — Support **four provider backends** in v1: dyndns2 (generic), DuckDNS,
  Cloudflare API, AWS Route53.
- G3 — **Zero-touch after configuration**: once enabled + credentialed, the
  device keeps its record fresh with no operator action across reboots and IP
  changes.
- G4 — Operator configures/enables DDNS from **device-ui**, including secret
  credentials handled write-only.
- G5 — Surface DDNS **health/state** (last IP, last success, last error) to
  device-ui and journald.
- G6 — Follow existing project conventions exactly (ACE reactor daemon, ds keys,
  systemd + preset + yocto packaging, secret-as-file/`LoadCredential`).

## 3. Non-goals (v1)

- N1 — Cloud-side DDNS for the Vultr VM's IP (separate initiative; the plan doc
  notes `cloud.dm.uri` is the insertion point there).
- N2 — IPv6 / AAAA records (fast-follow after v1).
- N3 — Registering / creating the DNS record or zone (operator pre-creates the
  hostname; DDNS only *updates* it).
- N4 — Solving CGNAT un-reachability (we publish the observed public IP; if it's
  CGNAT the name resolves but may be unreachable — we only *flag* this).
- N5 — Multiple hostnames per device / round-robin (single hostname per device).
- N6 — A DDNS management UI in the **cloud** console (device-ui only in v1).

## 4. Users & stories

- **Field operator**: "From the device-ui I pick a provider, enter my hostname
  and token, enable DDNS, and my device is reachable at that name — even after
  its IP changes."
- **Support engineer**: "I read `ddns.last.ip`, `ddns.state`, and
  `ddns.last.error` to confirm the record is fresh and diagnose failures without
  SSHing in."
- **Fleet integrator**: "I pre-seed the DDNS config + credential file in the
  image so devices self-register their name on first boot with no manual step."

## 5. Functional requirements

Each FR below becomes a tracked GitHub issue.

- **FR-1 — ds schema & config surface.** A new `ddns.*` data-store namespace
  (schema `ddns.lua`) defining config keys (`enabled`, `provider`, `hostname`,
  `interval`, `ip.source`), per-provider credential/target keys, and read-only
  runtime state keys (`state`, `last.ip`, `last.ok.ts`, `last.error`). Secret
  keys use the write-only access class. Hot-reloadable via ds watch.

- **FR-2 — `iot-ddnsd` daemon skeleton.** A new `modules/ddns/` module with an
  ACE single-reactor daemon mirroring cellular-client: connect ds, load config,
  `watch` config keys (react on the reactor thread via `handle_exception`),
  periodic `handle_timeout` at `ddns.interval`, publish state keys. No provider
  logic yet — logs the resolved config + detected IP.

- **FR-3 — public-IP detection.** A `PublicIpDetector` that resolves the current
  public IPv4 via HTTPS echo (`api.ipify.org`, `checkip.amazonaws.com`,
  `icanhazip.com`) with ordered fallbacks and a cache. Pluggable strategy
  (`echo` | `dyndns2-auto` | `cloud`); publishes `ddns.last.ip`. Only triggers a
  provider update when the IP changed (idempotent) or a forced-refresh window
  elapsed.

- **FR-4 — provider backend interface + dyndns2.** A `ProviderBackend` strategy
  interface (`update(hostname, ip, creds) -> Result`) and the **dyndns2** impl:
  HTTP Basic auth GET to `/nic/update`, parse `good`/`nochg`/error responses.
  This is the generic backbone (No-IP, Dynu, deSEC, IPv64, …).

- **FR-5 — DuckDNS backend.** Simple GET to `duckdns.org/update` with token; the
  first end-to-end smoke-test target (no real domain needed).

- **FR-6 — Cloudflare backend.** Bearer-token REST: resolve record id
  (`GET .../dns_records?name=`), then `PUT` the A record; JSON parse; cache the
  record id, re-resolve on 404.

- **FR-7 — Route53 backend.** AWS SigV4-signed `ChangeResourceRecordSets` UPSERT
  (HMAC-SHA256 canonical chain over the linked OpenSSL). Isolated so it can slip
  a phase without blocking others.

- **FR-8 — systemd + packaging.** `iot-ddnsd.service` in both
  `packaging/systemd/` and yocto `.../lwm2m/files/`; `.env` in
  `packaging/etc-iot/`; `90-iot.preset` enable line; `iot_git.bb` package split
  (`${PN}-ddns`, FILES/RDEPENDS/SYSTEMD_*); packagegroup entry; CMake wiring in
  the module + `apps/CMakeLists.txt`. No `RuntimeDirectory=iot`.

- **FR-9 — secrets handling.** Provider tokens/keys never committed and never
  echoed to viewers: settable via device-ui as write-only ds keys, and/or a
  pre-provisioned file (`/etc/iot/ddns/…`, `0640`) referenced by
  `LoadCredential=` / a `*.path` ds key. Follows the VPN-cert / BS-KEK pattern.

- **FR-10 — device-ui DDNS page.** A DDNS config page (Clarity `clr-datagrid` /
  4-column `.form-grid`, Admin-gated) to pick provider, set hostname/interval,
  enter write-only credentials, toggle enable, and a status tile showing
  `state` / `last.ip` / `last.ok.ts` / `last.error`.

- **FR-11 — reachability flag (CGNAT awareness).** After a successful update,
  optionally probe whether the published IP is reachable; set
  `ddns.state = ok` vs `ok-unreachable` so operators can tell a CGNAT'd device
  apart from a truly reachable one.

## 6. Non-functional requirements

- **NFR-1 — ACE-only.** No raw POSIX / std threading; all I/O on one
  `ACE_Reactor`. Logging via `ACE_DEBUG`/`ACE_ERROR` to journald.
- **NFR-2 — Resilience.** Exponential backoff + jitter on provider/echo errors;
  never hammer a provider (free tiers ban abuse). Survive ds restarts (systemd
  `Restart=on-failure`).
- **NFR-3 — Clock safety.** HTTPS to providers must tolerate the NTP-no-RTC
  cold-boot window ("cert not yet valid"): start after time is sane and/or
  retry; do not crash-loop.
- **NFR-4 — Security.** Secrets write-only / file-mode `0640`; scoped tokens
  recommended (Cloudflare Zone:DNS:Edit, Route53 least-priv). No secret in logs.
- **NFR-5 — Footprint.** Idle daemon negligible; single reactor thread; no busy
  polling — event- and timer-driven only.
- **NFR-6 — Testability.** Provider backends unit-tested against recorded
  responses; `PublicIpDetector` tested with stub echo; gtest under the module's
  `test/`.

## 7. Success metrics / acceptance

- A configured device updates its record within `ddns.interval` of an IP change,
  verified by external `dig`/`nslookup` resolving to the new IP.
- All four backends pass their unit tests; at least DuckDNS + dyndns2 pass a live
  HW end-to-end on RPi3B (Wi-Fi/home-NAT).
- Secrets never appear in ds read output, device-ui GET responses, or journald.
- `ddns.state`/`last.*` reflect reality and render in device-ui.

## 8. Rollout

- Feature ships **disabled by default** (`ddns.enabled=false`); opt-in per
  device via device-ui or image seed.
- Phased per the TDD (schema+skeleton → detection → dyndns2/DuckDNS →
  Cloudflare → Route53 → device-ui → HW validation).

## 9. Open questions

- Q1 — Default forced-refresh window (guard against out-of-band edits / free-tier
  "confirm every 30 days" expiry)? Proposal: 24h.
- Q2 — Should the daemon also write AAAA when a global IPv6 is present, or defer
  entirely to v2? Proposal: defer.
- Q3 — Is cloud-fed public IP (`iot.wan.ip` pushed from the cloud's observed
  `wan_ip`) worth it as a detection source, or is echo sufficient? Proposal:
  echo for v1, revisit.
- Q4 — Per-device hostname naming convention (`dev-<serial>.<domain>` vs
  operator-free-form)? Proposal: operator-free-form, with `<serial>` suggested.
