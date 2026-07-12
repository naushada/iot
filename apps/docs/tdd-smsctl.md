# TDD — SMS device control (`iot-smsctld`)

*2026-07-12. Epic #533. Plan: `smsctl-plan.md`. PRD: `prd-smsctl.md`.*

## Implementation status

| PR | FR | Issue | Scope | Status |
|----|----|-------|-------|--------|
| PR-1 | FR-1, FR-2, FR-3 | #534 #535 #536 | module, `smsctl.lua`, parser, session/auth engine, parked daemon | ☐ |
| PR-2 | FR-4, FR-5, FR-7, FR-8 | #537 #538 #540 #541 | reboot / factory-reset / APN / radio / STATUS executors + reply channel | ☐ |
| PR-3 | FR-6 | #539 | WiFi SSID/PSK executor + wifi-client `wifi.networks` hot-apply | ☐ |
| PR-4 | FR-9, FR-10 | #542 #543 | packaging + device-ui card | ☐ |

## Goal + v1 scope

Let an operator with a phone control a device that has **no IP path** — the
only surviving channel is MT-SMS on the WP7702. `iot-smsctld` consumes the
existing MT envelope, authenticates the sender, executes one of a fixed
allowlist of commands, and answers with one MO SMS.

In scope: `LOGIN`, `LOGOUT`, `STATUS`, `REBOOT`, `FACTORY-RESET` (nonce),
`APN`, `RADIO RESTART`, `WIFI <ssid> <psk>`.
Out of scope: shell-over-SMS, multipart commands, binary/PDU commands,
cloud-originated SMS, delivery receipts.

## Locked decisions

1. **Separate daemon, not inside cellular-client.** cellular-client stays
   modem I/O only; smsctld owns policy. Coupling is via the existing,
   HW-proven `sms.*` ds envelopes — no new modem surface.
2. **ds IS the command bus.** smsctld never performs a device action itself:
   it writes the ds key that the **owning daemon** already watches, and that
   daemon acts. `APN`/`RADIO RESTART` → `cell.apn` + `cell.reset.request`
   (cellular-client cycles the radio). `WIFI` → `wifi.networks`
   (wifi-client re-applies). smsctld owns no modem, no wpa_supplicant, no
   nft. The only exceptions are `REBOOT` and `FACTORY-RESET`, which have **no
   owning daemon** — there the root `.path` unit *is* the module, and the
   trigger file under `/run/iot` is the repo's established privilege bridge
   (exactly what iot-httpd does for the device-ui buttons).
3. **Unprivileged daemon, `Group=iot`** (mirrors `iot-ddnsd`). Group `iot`
   gives it the ds socket and write access to `/run/iot` (2775 root:iot) —
   nothing more. We do NOT run it as root.
   *Corollary:* this forced a real fix rather than a workaround — `wifi.networks`
   had **no hot-apply** (wpa_supplicant reads its config once, at spawn), so the
   first design bounced `services.wifi.client.enable`, which is
   `write_acl={"uid:0"}` and unreachable from an unprivileged daemon. Instead of
   inventing a privileged side channel, **wifi-client now watches
   `wifi.networks` and re-applies within one 200 ms tick** — which also fixes
   the long-standing device-ui wart that a WiFi credential change needed a
   manual service restart.
4. **`IOT ` prefix is mandatory.** Anything else (carrier spam, human text)
   is dropped with **zero MO traffic**. Non-E.164 (alphanumeric) senders are
   never replied to — they cannot receive SMS and it would burn credit.
5. **Auth is the password, not the sender number.** SMS sender IDs are
   spoofable on some routes; the allowlist is a coarse pre-filter, the
   `sha256` password check (reusing `auth.users.*`) is the actual gate.
   Password transits the carrier in plaintext → feature is **default-off**
   and documented as "use a dedicated account".
6. **Sessions are per-MSISDN, in-memory, TTL-bounded** (default 600s). A
   daemon restart drops sessions (by design — no persisted auth state).
7. **FACTORY-RESET needs a two-step nonce** (6 digits, 5-min, single use).
   The single most destructive command must not be one typo away.
8. **APN applies via the radio cycle**, reusing `cell.reset.request` (PR
   #532). Requires the one-line cellular-client fix below.
9. **Replay-safe by baseline.** At startup the daemon reads `sms.version` +
   `sms.last.*` and records them as *already seen*; stored SMS drained from
   the SIM at boot can never execute.

## Command grammar

```
IOT LOGIN <user> <password>
IOT LOGOUT
IOT STATUS
IOT REBOOT
IOT FACTORY-RESET                 → reply carries a 6-digit nonce
IOT FACTORY-RESET <nonce>         → executes
IOT APN <apn>
IOT RADIO RESTART
IOT WIFI <ssid> [<psk>]           → psk omitted = open network
IOT WIFI "<ssid with spaces>" "<psk with spaces>"
```

- `IOT` prefix and keywords are **case-insensitive**; arguments are **not**
  (SSIDs and PSKs are case-sensitive).
- One command per SMS. Leading/trailing whitespace ignored.
- Double quotes group an argument containing spaces; `\"` escapes a quote.
- Replies: `OK <CMD>[: <detail>]` or `ERR <CMD>: <reason>`, ASCII, ≤160
  chars (one GSM-7 SMS; the reply is truncated with `…` if longer).

### Reply contract — exactly one SMS per command, success **or** failure

Every command that is parsed from an allowed sender is answered. Auth failures
are answered too: an operator who mistypes a password must be told, not left
guessing whether the device is even alive.

| inbound | reply |
|---|---|
| `IOT LOGIN <user> <right-pw>` | `OK LOGIN: admin, 10 min` |
| `IOT LOGIN <user> <wrong-pw>` | `ERR LOGIN: invalid credentials` |
| …after `smsctl.lockout.failures` tries | `ERR LOGIN: locked out (15 min)` |
| `IOT LOGIN admin` (bad arity) | `ERR: usage: IOT LOGIN <user> <password>` |
| any command with no live session | `ERR <CMD>: login required (IOT LOGIN <user> <password>)` |
| mutating command as a Viewer | `ERR <CMD>: admin access required` |
| `IOT <junk>` | `ERR: unknown command` |
| anything that succeeds | `OK <CMD>[: <detail>]` |

**Four deliberate silences** — each is a safety property, not an oversight:

1. text without the `IOT ` prefix (human messages, carrier spam) → **zero MO
   traffic**, and no reply loop;
2. a sender not on `smsctl.allowed.numbers` → silent, so the device is not an
   oracle for "is smsctl on here?" and spam cannot burn SMS credit;
3. an alphanumeric sender (`AZ-AIRTEL-S`) → it physically cannot receive SMS;
4. `smsctl.enabled=false` → the daemon is parked.

Never echo an argument back into a reply: a parse error must not quote the
password it just failed to parse.

| Command | Access | Session | Effect |
|---|---|---|---|
| `LOGIN` | — | starts one | verify sha256 → session (TTL) |
| `LOGOUT` | any | ends it | drop session |
| `STATUS` | Viewer+ | required | read-only summary |
| `REBOOT` | Admin | required | `/run/iot/reboot.request` |
| `FACTORY-RESET` | Admin | required | nonce → `/run/iot/factory-reset.request` |
| `APN` | Admin | required | `cell.apn` + `cell.reset.request` |
| `RADIO RESTART` | Admin | required | `cell.reset.request` |
| `WIFI` | Admin | required | `wifi.networks` upsert (wifi-client hot-applies) |

## Components

```
modules/smsctl/
  inc/smsctl/
    command.hpp      Command struct + CommandKind enum
    parser.hpp       parse(text) -> Command            (pure)
    session.hpp      SessionStore: login/lockout/nonce (pure, clock-injected)
    executor.hpp     Executor + DsSink interface       (pure over a sink)
  src/
    parser.cpp  session.cpp  executor.cpp
  daemon/
    main.cpp  smsctl_client.cpp/.hpp   ACE reactor daemon
  schemas/smsctl.lua
  test/  parser_test.cpp session_test.cpp executor_test.cpp
  CMakeLists.txt        (SMSCTL_BUILD_DAEMON option, default OFF)
```

`DsSink` is a tiny interface (`set(key,val)`, `get(key)`, `arm_trigger(path)`,
`reply(to,text)`) so **every executor is unit-testable with a mock sink** —
no ds server and no modem in the tests. The daemon supplies the real sink.

## Data-store keys (`schemas/smsctl.lua`)

Config (Admin):
| key | type | default | meaning |
|---|---|---|---|
| `smsctl.enabled` | boolean | `false` | master switch; daemon parks when false |
| `smsctl.allowed.numbers` | string | `""` | CSV E.164 allowlist; empty = any sender may attempt LOGIN |
| `smsctl.session.ttl.sec` | integer | `600` | session lifetime (60…86400) |
| `smsctl.lockout.failures` | integer | `5` | failed logins per number before lockout (1…20) |
| `smsctl.lockout.sec` | integer | `900` | lockout window (60…86400) |

Status (Viewer, volatile):
| key | meaning |
|---|---|
| `smsctl.state` | `disabled` / `listening` |
| `smsctl.last.sender` | last sender that issued a *valid-prefix* command |
| `smsctl.last.cmd` | last command **keyword only** (never args — a PSK/password must never land in ds) |
| `smsctl.last.result` | `ok` / `err: <reason>` |
| `smsctl.last.ts` | epoch seconds |
| `smsctl.sessions` | count of live sessions |
| `smsctl.version` | bump-on-change for the device-ui long-poll |

**Reused keys** (no new surface): `sms.version`, `sms.last.{sender,text,ts}`
(inbound), `sms.send.{to,text,request}` (reply), `cell.apn`,
`cell.reset.request`, `wifi.networks`, `auth.users.admin.password.hash`,
`auth.users.accounts`, plus read-only `cell.*`/`vpn.state`/`wifi.assoc.ssid`/
`net.iface.active` for STATUS.

## Daemon internals

Reactor daemon (`cellular_client` idiom):

```
run():
  ds.connect()
  load_config_from_ds()                 // smsctl.* knobs
  baseline: read sms.version + sms.last.*  → mark seen   (replay guard)
  watch({sms.version, smsctl.enabled, smsctl.allowed.numbers, ...})
       → listener thread: set dirty flag + reactor()->notify(this)
  schedule_timer(1s repeating)          // session/nonce expiry sweep
  run_reactor_event_loop()

handle_exception():                     // reactor thread
  if config dirty  → reload knobs, publish smsctl.state
  if sms dirty     → drain_inbound()

drain_inbound():
  read sms.last.{sender,text,ts}
  if (sender,ts,text) == last_seen → return          // dedupe
  if !smsctl.enabled                → return         // parked
  if allowlist non-empty && sender not in it → return (SILENT, no reply)
  cmd = parser::parse(text)
  if cmd.kind == NotACommand        → return         (SILENT)
  reply = engine.handle(cmd, sender, now)            // auth + execute
  if sender is E.164 → sink.reply(sender, reply)
  publish smsctl.last.* (keyword + result only)
```

The 1s timer only expires sessions/nonces (no modem traffic). The watch
callback never touches modem/ds state directly — it flags and notifies,
exactly like `on_send_request` in cellular-client.

### Executors → the exact writes

| command | writes |
|---|---|
| `REBOOT` | create `/run/iot/reboot.request` (content `reboot\n`) |
| `FACTORY-RESET <nonce>` | create `/run/iot/factory-reset.request` |
| `APN <a>` | `set cell.apn=<a>`; `set cell.reset.request=<epoch_ms>` |
| `RADIO RESTART` | `set cell.reset.request=<epoch_ms>` |
| `WIFI <s> <p>` | read `wifi.networks` → upsert `{ssid,psk,key_mgmt}` (replace same-`ssid`, else append; empty psk → `key_mgmt:"NONE"`, drop `psk`) → `set wifi.networks`. **That write is the whole command** — wifi-client watches the key and respawns wpa_supplicant with the new credentials. |
| `STATUS` | reads only |

### Prerequisite fix in cellular-client (FR-5)

`start_reset()` (PR #532) cycles the radio but does **not** re-read config, so
a freshly-written `cell.apn` would not be applied by the cycle. One line:

```cpp
void CellularClient::start_reset() {
    if (!m_at) return;
    load_config_from_ds();     // pick up a cell.apn/cell.rat changed since boot
    ...
}
```

`m_apn_sent=false` (already in `start_reset`) then re-issues `AT+CGDCONT` with
the new APN on the next poll.

### Prerequisite fix in wifi-client (FR-6) — `wifi.networks` hot-apply

`wpa_supplicant` reads its config **once, at spawn**, so new credentials do
nothing until the workers are re-spawned. Today `wifi.networks` therefore has no
hot-apply: an operator who fixes a mistyped PSK (from the device-ui *or* by SMS)
sees nothing happen until someone restarts the service — which is impossible on
the device they just locked themselves out of.

`DsBridge` already watches `wifi.networks` and fires `on_change(Key::Networks)`.
Wire it into `Supervisor`:

```cpp
// ctor — listener thread: flag only
m_ds.on_change([this](DsBridge::Key k) {
    if (k == DsBridge::Key::Networks) m_networks_dirty.store(true);
});

// run loop — reap + reinitialize() within one 200ms tick (same idiom the
// service-disable path already uses; initialize() re-reads wifi.networks,
// rewrites wpa_supplicant.conf and respawns).
if (m_networks_dirty.exchange(false)) {
    m_dhcp.terminate(); m_wpa.terminate(); m_ctrl.close();
    m_ds.set_assoc_state("disconnected");
    if (!initialize()) continue;   // bad JSON → state=conflict, stay up
    ...
}
```

Safe to re-spawn: `nm_conflict_detected()` reclaims a stale ctrl socket (PR
#222), and a malformed `wifi.networks` leaves the daemon **up** with
`assoc.state=conflict` + `wifi.last.error` rather than exiting — an operator typo
must not kill WiFi permanently.

This removes the need for any privileged WiFi side channel from smsctld.

## Yocto / image prereqs

- Unit `iot-smsctld.service` in **both** `packaging/systemd/` and
  `yocto/meta-iot/recipes-iot/lwm2m/files/`; `.env` in `packaging/etc-iot/`.
  `Group=iot`, `After=network-online.target iot-ds.service`,
  `Restart=on-failure`, hardening block, **no `RuntimeDirectory=iot`**.
- `iot_git.bb`: `SRC_URI += file://iot-smsctld.service`; `PACKAGE_BEFORE_PN +=
  ${PN}-smsctl`; `FILES:${PN}-smsctl` = binary + unit; `RDEPENDS:${PN}-smsctl =
  "ace-tao openssl"`; `SYSTEMD_SERVICE/AUTO_ENABLE:${PN}-smsctl = "enable"`.
- `90-iot.preset`: `enable iot-smsctld.service` (preset gotcha #394).
- `packagegroup-iot.bb`: add `iot-smsctl`.
- No new root units: WiFi is applied by wifi-client itself; reboot/factory-reset
  reuse the existing `iot-reboot.path` / `iot-factory-reset.path`.
- `apps/CMakeLists.txt`: `set(SMSCTL_BUILD_DAEMON ON CACHE BOOL "" FORCE)` +
  `add_subdirectory(modules/smsctl)` + install unit/env (forcing the daemon ON
  here is what makes the image build it — it MUST then be packaged or
  `do_package_qa` fails on installed-but-not-shipped).

## device-ui (FR-10)

Admin card on **WAN → Cellular Config**: `smsctl.enabled` toggle, allowed
numbers, session TTL, in the 4-column `.form-grid`; a status line
(`smsctl.last.cmd` / `.result` / `.ts`). Add every `smsctl.*` config key to
`DataStoreService.ALL_KEYS` — omitting them is exactly the bug that made the
"Receive SMS" checkbox forget its value across a reload (fixed in #532).

**Gated on a real modem (#549).** The whole card — including *Allowed Numbers* —
is disabled behind an explanatory banner until `cell.state` reaches
`registered` / `connected`. SMS control's only inbound channel is MT-SMS, which
needs network registration, so offering the fields on a device with no modem (or
one that never registered) invites an operator to "enable" a feature that
silently cannot work.

## Phasing (merge order)

PR-1 (schema + parser + session + parked daemon, all gtested) → PR-2
(executors + reply channel + the cellular `start_reset` reload) → PR-3 (WiFi executor +
wifi-client hot-apply) → PR-4 (packaging + UI). Each PR is independently
green; the feature is inert until `smsctl.enabled=true`.

## Testing

Unit (podman `ubuntu:22.04`, no HW):
- parser: every command, quoted args with spaces, wrong arity, junk, non-`IOT`
  text, case-insensitivity, password never echoed in error text.
- session: login ok/bad, TTL expiry, lockout after N + recovery after window,
  allowlist drop, Viewer-vs-Admin gating, nonce expiry/reuse/mismatch.
- executor (mock sink): exact ds writes/trigger paths per command; WiFi upsert
  (new / replace-same-SSID / open network / malformed existing JSON); STATUS
  ≤160 chars with missing keys rendered `-`.
- replay guard: same (sender,ts,text) processed once.

HW recipe (RPi3B + WP7702, from a phone):
1. `ds-cli set smsctl.enabled true`; `ds-cli set smsctl.allowed.numbers '"+9199…"'`;
   `systemctl restart iot-smsctld`.
2. Text `IOT STATUS` → expect `ERR STATUS: login required`.
3. `IOT LOGIN admin <pw>` → `OK LOGIN: admin, 10 min`. Then `IOT STATUS` → summary.
4. `IOT WIFI "Home" <psk>` → `OK WIFI: "Home" saved, reconnecting`; wifi-client
   respawns wpa_supplicant within ~200ms — confirm `wifi.assoc.ssid` flips on the
   device-ui with NO service restart.
5. `IOT APN airtelgprs.com` → `OK APN` and `cell.apn.current` updates after the cycle.
6. `IOT RADIO RESTART` → `cell.reg.cs` returns to `home` (the 2026-07-11 CS-wedge recovery).
7. `IOT FACTORY-RESET` → nonce reply; `IOT FACTORY-RESET <nonce>` → device wipes + reboots.
8. Wrong password ×5 → `ERR LOGIN: locked out (15 min)`; verify no further replies.
9. Send carrier-spam-like text → **zero** MO SMS (check `sms.send.request` unchanged).
10. `systemctl restart iot-smsctld` with SMS in the SIM store → nothing executes.

## Security

- Default **off**; enable per device.
- Password in plaintext over the carrier — documented; recommend a dedicated
  Admin account for SMS, not the shared `admin`.
- `smsctl.last.cmd` stores the **keyword only** — never arguments (a PSK or
  password must never reach ds or the journal). Log lines redact args to `***`.
- Lockout (5 failures / 15 min per MSISDN) throttles brute force; the carrier's
  own latency makes SMS brute force impractical anyway.
- Allowlist drops are silent → no oracle for "is this device listening?", and
  no reply-loop with alphanumeric senders.
- The daemon gains **no new privilege**: everything runs through existing ds
  ACLs and root `.path` trigger units.

## Open items

- OQ-1 `sms.last.*` is last-wins: two commands arriving within one poll window
  → the first is lost. Fix (v2): an `sms.rx.*` FIFO envelope in cellular-client.
- OQ-2 `sms.send.*` collides with a device-ui-initiated send (last writer wins).
  v1 accepts it (single-operator device); v2 could give smsctld its own lane.
- OQ-3 audit trail of SMS commands to the cloud (LwM2M notify / `smsctl.audit`).
- OQ-4 `IOT VPN RESTART` / `IOT OTA <version>` — natural next commands, not v1.
