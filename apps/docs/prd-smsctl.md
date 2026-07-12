# PRD — SMS device control (smsctl)

*2026-07-11. Plan: `smsctl-plan.md`. TDD: `tdd-smsctl.md`.*

## Problem

A deployed device with broken IP connectivity (bad WiFi PSK, wrong APN,
wedged radio, VPN down) is unreachable by device-ui, cloud and SSH. The
WP7702 SMS planes (MT receive + MO send, HW-proven) are the only remaining
channel, but the device cannot act on an SMS today.

## Goals

- Authenticated remote control of a small, curated command set over SMS.
- Human-typable command format from any phone; one reply SMS per command.
- Reuse existing device auth (admin hash + `auth.users.accounts`) and route
  every command through ds to the daemon that owns that domain; only
  reboot/factory-reset (which no daemon owns) use the root `.path` triggers.
- Off by default; safe against carrier spam, reply loops and casual abuse.

## Non-goals

- Cloud-originated SMS (no SMS gateway integration).
- Multipart command SMS, binary/PDU-structured commands, delivery receipts.
- Full shell over SMS; the command set is a fixed allowlist.
- Guaranteeing MT delivery (carrier/CS-registration dependent).

## User stories

- As a field operator, I text `IOT LOGIN svc <pw>` then `IOT WIFI "Site AP"
  <psk>` to fix a mistyped WiFi password without driving to the site.
- As an operator, I text `IOT STATUS` and get registration/signal/IP/VPN
  state back in one SMS.
- As an admin, I text `IOT REBOOT` to recover a wedged device, or walk
  through the nonce handshake for `IOT FACTORY-RESET`.
- As a security owner, I keep smsctl disabled fleet-wide except on devices
  where I explicitly enable it with a sender allowlist.

## Functional requirements

- **FR-1 — Module + schema + parked daemon.** `modules/smsctl/` with
  `schemas/smsctl.lua` (`smsctl.enabled` default false, `smsctl.allowed.numbers`,
  `smsctl.session.ttl.sec`, lockout knobs, status keys) and an `iot-smsctld`
  reactor-daemon skeleton that connects ds, parks until `smsctl.enabled`,
  and watches `sms.version`.
- **FR-2 — Command parser (pure).** Tokenise + parse the `IOT …` grammar
  (case-insensitive keywords, quoted args, one command per SMS) into a typed
  `Command`; reject non-`IOT` text silently. gtested.
- **FR-3 — Session + auth engine (pure).** `LOGIN` verifies
  `sha256(password)` against the admin hash or an `auth.users.accounts`
  entry; session binds the sender MSISDN with TTL; `LOGOUT`; per-number
  failed-login lockout; optional sender allowlist (silent drop); Viewer may
  only `STATUS`, mutating commands need Admin. Passwords never logged or
  echoed. gtested.
- **FR-4 — Reboot + factory-reset executors.** `REBOOT` arms
  `/run/iot/reboot.request`. `FACTORY-RESET` is two-step: first message
  replies with a 6-digit nonce (5-min expiry, single use); `FACTORY-RESET
  <nonce>` arms `/run/iot/factory-reset.request`.
- **FR-5 — APN + radio executors.** `APN <apn>` sets `cell.apn` and bumps
  `cell.reset.request`; `RADIO RESTART` bumps `cell.reset.request`.
  Includes the cellular-client fix: `start_reset()` re-runs
  `load_config_from_ds()` so the new APN is applied by the cycle.
- **FR-6 — WiFi executor + wifi-client hot-apply.** `WIFI <ssid> <psk>` upserts
  the entry in the `wifi.networks` JSON array (PSK shape `{ssid, psk,
  key_mgmt:"WPA-PSK"}`; empty psk → `key_mgmt:"NONE"`). The ds write IS the
  command: **iot-wifi-client now watches `wifi.networks`** and re-applies
  (respawns wpa_supplicant) within one 200ms tick. Previously the key had no
  hot-apply, so a credential change silently did nothing until a service
  restart — which also fixes the same wart for the device-ui.
- **FR-7 — STATUS executor.** One GSM-7 SMS ≤160 chars from ds:
  `cell.reg`/`cell.reg.cs`, `cell.signal.dbm`, `cell.ip`, `vpn.state`,
  `wifi.assoc.ssid`, `net.iface.active`.
- **FR-8 — Reply channel.** Every processed command (auth failures included,
  allowlist drops excluded) produces exactly one `OK/ERR` reply via the
  `sms.send.*` envelope; replies only to E.164 senders (never alphanumeric).
- **FR-9 — Packaging.** systemd unit (both copies) + `.env`, `iot_git.bb`
  package `${PN}-smsctld`, `90-iot.preset`, `packagegroup-iot.bb`,
  `apps/CMakeLists.txt` wiring. Unit ships enabled, daemon parked.
- **FR-10 — device-ui card.** Admin card on the Cellular config page:
  enable toggle, allowed numbers, session TTL; status line with last
  command/result (`smsctl.last.*`).

## Non-functional requirements

- NFR-1: replies fit one GSM-7 SMS (≤160 chars); ASCII-only reply text.
- NFR-2: no plaintext password in journal, ds, or replies (redact to `***`).
- NFR-3: a non-allowlisted or alphanumeric sender causes zero MO traffic.
- NFR-4: daemon is ACE-reactor only (no raw POSIX threading); all state
  in-memory + volatile ds mirrors; restart-safe (no replay of old SMS).
- NFR-5: unit-test coverage for parser, session/lockout/nonce state machine
  and executors' ds-write shapes (mock sink).

## Success metrics

- e2e on HW: login + each command executed from a phone, one reply each,
  wrong-password lockout verified, stale-SMS replay verified impossible
  across daemon restart.
- Zero MO SMS sent for carrier spam / non-IOT inbound traffic.

## Rollout

Default-off ds key; ship enabled-but-parked daemon in the standard image.
Enable per device: `smsctl.enabled=true` (+ allowlist) via device-ui/cloud.

## Open questions

- OQ-1: `sms.last.*` last-wins race under burst inbound — v2 `sms.rx.*`
  queue envelope in cellular-client?
- OQ-2: should `sms.send.*` get a dedicated `smsctl` lane to avoid clobbering
  an operator's in-flight UI send? (v1: last-writer wins, documented.)
- OQ-3: per-command audit trail to cloud (`smsctl.audit` → LwM2M notify)?
