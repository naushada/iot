# SMS device control (smsctl) — plan

*2026-07-11. Status: planned. PRD: `prd-smsctl.md`, TDD: `tdd-smsctl.md`.*

## Motivation

A fielded device is often reachable ONLY over the cellular modem: WiFi
misconfigured, VPN down, cloud unreachable, or the device deployed before any
IP provisioning. MT-SMS is the lowest-common-denominator back channel — it
works with zero IP connectivity, on a 2G-only RAT, and from any phone. The
WP7702 SMS planes are already built and HW-proven (MT receive PR-A/B/C, MO
send PR-E, CS-wedge diagnostics + Restart Module 2026-07-11): the device can
receive and send SMS today. What's missing is acting on them.

Target scenarios:
- Device lost WiFi (typo'd PSK) → text `IOT WIFI "Home" newpass` to fix it.
- Wrong APN after a SIM swap → `IOT APN airtelgprs.com`.
- Cellular data wedged (CS/PS reg wedge) → `IOT RADIO RESTART`.
- Device wedged beyond remote diagnosis → `IOT REBOOT`, last resort
  `IOT FACTORY-RESET`.
- Quick health poll with no data plan → `IOT STATUS`.

## Architecture

One new **reactor daemon `iot-smsctld`** (`modules/smsctl/`), decoupled from
the modem by the existing ds envelopes:

```
              MT SMS                     MO SMS reply
  modem ──> cellular-client ──> ds ──> iot-smsctld ──> ds ──> cellular-client ──> modem
            (sms.last.*,                (parse, auth,         (sms.send.* envelope,
             sms.version bump)           execute)              AT+CMGS)
```

- **Inbound**: watch `sms.version`; on bump read `sms.last.{sender,text,ts}`.
  Dedupe on the (sender, ts, text) tuple; baseline at startup so stored/stale
  messages are never replayed as commands.
- **Execute**: ds is the command bus. smsctld writes the key the OWNING daemon
  already watches, and that daemon acts — smsctld owns no modem, no
  wpa_supplicant, no nft. Reboot/factory-reset are the only exceptions (no
  daemon owns them: the root `.path` unit is the module, exactly as device-ui
  does it). No new privileged surface:
  - reboot → `/run/iot/reboot.request` (root `iot-reboot.path` acts)
  - factory-reset → `/run/iot/factory-reset.request` (nonce-confirmed)
  - APN → `set cell.apn` + bump `cell.reset.request` (radio cycle applies it;
    needs the one-line cellular-client fix: `start_reset()` reloads config)
  - radio restart → bump `cell.reset.request` (built 2026-07-11, PR #532)
  - WiFi → edit the `wifi.networks` JSON array; **wifi-client hot-applies it**
    (new: the daemon watches the key and respawns wpa_supplicant — previously
    a credential change needed a manual service restart, which is impossible
    on the device you just locked yourself out of)
- **Reply**: write `sms.send.{to,text}` + bump `sms.send.request` — the
  proven MO path. One reply SMS per inbound command, ≤160 GSM-7 chars.

### Why a separate daemon (not inside cellular-client)?
- cellular-client stays a pure modem I/O daemon (AT/NMEA → ds); command
  policy, sessions and lockouts are a different concern with different
  failure modes.
- smsctld is transport-agnostic: v2 could feed the same parser from a cloud
  push or LwM2M execute without touching the modem daemon.
- The ds envelopes already exist and are HW-proven — zero new coupling.

### Options considered
- **In cellular-client**: fewer moving parts, but mixes policy into the modem
  daemon and can't be reused for other command transports. Rejected.
- **Cloud-side command relay** (cloud sends SMS via an SMS gateway): needs a
  paid gateway + the cloud to be reachable — defeats the purpose (SMS is the
  no-IP fallback). Out of scope; the on-device daemon still works with it.
- **Structured/binary SMS (PDU port addressing, SIM toolkit)**: powerful but
  not human-typable from a phone. The whole point is "operator with a phone".

## Command format (v1)

Human-typable, one command per SMS, `IOT ` prefix so ordinary messages and
carrier spam are never interpreted:

```
IOT LOGIN <user> <password>       start a session (sender-number bound, TTL)
IOT LOGOUT                        end the session
IOT STATUS                        one-line health (reg/signal/ip/vpn/wifi)
IOT REBOOT                        reboot the device
IOT FACTORY-RESET                 step 1 → reply carries a 6-digit nonce
IOT FACTORY-RESET <nonce>         step 2 → executes (nonce valid 5 min)
IOT APN <apn>                     set cell.apn + radio cycle to apply
IOT RADIO RESTART                 CFUN 0/1 cycle (cell.reset.request)
IOT WIFI <ssid> <psk>             set/replace the WiFi network + reconnect
IOT WIFI "<ssid with spaces>" "<psk>"
```

Replies: `OK <CMD>[: detail]` / `ERR <CMD>: <reason>`. Examples:
`OK LOGIN: admin, 10 min`, `ERR LOGIN: invalid credentials`,
`OK WIFI: "Home" saved, reconnecting`, `OK STATUS: reg=home sig=-89dBm
ip=100.127.175.251 vpn=up wifi=Home`.

## Security posture

SMS has two hard limitations we design around, not away:
1. **Sender spoofing is possible** on some routes → sender-number binding is
   a convenience, not the authentication. Authentication is the login
   password (existing device users, `sha256` verify).
2. **The password transits the carrier in plaintext** → opt-in feature
   (default off), dedicated low-privilege account recommended, password never
   echoed in replies or logs.

Layered gates: `smsctl.enabled` (default **false**) → optional sender
allowlist (non-listed numbers ignored **silently**, no reply — also prevents
reply loops with alphanumeric senders, which are never replied to) → login
session (TTL default 10 min) → per-number lockout after 5 failed logins →
Admin-access account required for mutating commands (Viewer may `STATUS`) →
factory-reset nonce double-confirmation.

## Phasing

1. Schema + parser/session engine (pure, gtested) + daemon skeleton (parked
   until `smsctl.enabled`).
2. Executors: reboot, radio restart, APN (incl. cellular-client
   `start_reset()` config reload), STATUS.
3. Executors: WiFi, factory-reset (nonce).
4. Packaging (systemd + yocto + preset + packagegroup) and device-ui card.

## Risks

- **Reply-loop / SMS cost**: bound replies to 1 per inbound, never reply to
  non-`IOT` messages or alphanumeric senders, lockout throttles abuse.
- **sms.last.* races**: two SMS in the same poll window — v1 accepts
  last-wins (SMS arrival is slow); TDD notes an `sms.rx.*` envelope as the
  v2 fix.
- **sms.send collision with device-ui sends**: both bump `sms.send.request`;
  last-writer wins. Acceptable v1 (single-operator device), noted in TDD.
- **Carrier delivery**: MT-SMS needs CS registration (see the 2026-07-11
  CS-wedge finding) — `IOT RADIO RESTART` is itself the recovery, but only
  if delivery works; the ds-reconnect + per-domain reg UI make this visible.
