# TDD Plan — Receive SMS over the mangOH NB-IoT Cellular modem

Status: **CODE COMPLETE (needs HW)** — PR-A (`sms_pdu` decoder), PR-B
(`SmsReceiver` + reassembler + `sms.*` schema + `CellularState::set_sms`) and
PR-C (daemon wiring + device-ui tile + Yocto) all implemented and podman-built
(44/44 cellular gtests, `iot-httpd` links clean, warning-clean). Off by default
(`sms.enable`); the remaining gate is the on-hardware validation in §6 #1 (does
the NB-IoT SIM/carrier actually deliver an MT SMS). Extends the cellular WAN plane
(`[[project_mangoh_yellow_sensors]]`, `apps/docs/tdd-mangoh-yellow-sensors.md`
§6) with **mobile-terminated (MT) SMS reception** off the same CF3 / WP module,
reusing the existing `cellular_core` + `cellular-client` split. Send (MO SMS) is
explicitly out of scope (see §7).

## 0. Goal & motivation

The mangOH Yellow's Sierra WP7702 (NB-IoT / CAT-M) can receive SMS over the same
AT control channel the `cellular-client` daemon already owns (`/dev/ttyUSB2`).
An inbound SMS is useful as:

1. **An out-of-band control/wake channel** — reach a device that has no data
   context (no APN activated, VPN down, roaming-data barred) but is still
   camped on the network. NB-IoT SIMs frequently ship data-capped but
   SMS-enabled.
2. **Operator/provisioning nudges** — carrier or fleet-owner messages surfaced
   in the device-ui and (optionally) forwarded to cloud-iot.

This TDD covers **receive → decode → publish to the data-store → device-ui**,
plus an optional **cloud forward**. Acting on an SMS (command execution) is
called out as a follow-up, not built here.

> ⚠️ **NB-IoT SMS is operator- and firmware-gated.** SMS on NB-IoT may ride the
> control plane (SMS-over-NAS / non-IP) and some NB-IoT SIMs ship with MT-SMS
> disabled or MO-only. **Before any of the PRs below, HW open-question #1 (§6)
> must be answered:** run the AT sequence by hand and confirm a real MT SMS
> arrives. If the carrier drops it, no amount of parser is going to help.

## 1. Architecture (fits the existing 2-layer cellular split)

```
 cellular-client daemon (ACE, owns the AT fd)          modules/wan/cellular/daemon/
   run()          ── one-time SMS setup after vendor detect:
                     AT+CMGF=0 (PDU), AT+CNMI=2,1,… , AT+CPMS, AT+CMGL=4 (drain)
   on_at_line()   ── feeds each AT line to SmsReceiver; issues the commands it
                     returns (AT+CMGR=<i> / AT+CMGD=<i>); publishes decoded msgs
   ─────────────────────────────────────────────────────────────────────────────
 cellular_core (pure, host-testable)                   modules/wan/cellular/{inc,src}/
   SmsReceiver     ── stateful line→command/message machine (URC → CMGR → PDU)
   sms_pdu         ── GSM 03.40 SMS-DELIVER PDU decode (7-bit/UCS2, UDH concat)
   SmsReassembler  ── multipart (concatenated) SMS reassembly by (ref,total)
   CellularState   ── += sms.* fields in to_kv() (sms.last.*, sms.count, version)
   schemas/cell.lua── += sms.* keys
```

The key architectural move: **keep the daemon a thin reactor shell** exactly as
today. All the SMS logic that has interesting behaviour — URC state machine, PDU
decode, multipart reassembly — lives in **new pure `cellular_core` units** with
no ACE/I/O, unit-tested on the host. This mirrors how `at_parser` / `nmea_parser`
/ `line_router` already relate to `SerialChannel` / `CellularClient`.

## 2. Why the current `line_router` can't just grow an `if`

`dispatch_at_line()` (`src/line_router.cpp:34`) is a chain of **stateless
one-line matchers** — each `+CSQ:` / `+COPS:` / `+CEREG:` line is self-contained.
SMS reception is **not** one line and **not** stateless:

- **Two lines per read.** `AT+CMGR` replies with a `+CMGR: <stat>,<alpha>,<len>`
  header line, then the **PDU (or text body) on the *next* line**. The router
  must remember "a PDU is coming next".
- **URC-driven, asynchronous.** `AT+CNMI=2,1,…` makes the modem emit an
  unsolicited `+CMTI: "SM",<index>` when a message lands — arriving *between*
  our polls, interleaved with `+CSQ`/`+CEREG` replies. Handling it means
  **issuing a follow-up command** (`AT+CMGR=<index>`) from within line handling,
  then a cleanup (`AT+CMGD=<index>`).
- **`LineAssembler` drops empty lines** (`line_router.cpp:27`, `if
  (!line.empty())`). A text-mode (`CMGF=1`) body containing a blank line would be
  silently mangled. **PDU mode (`CMGF=0`) sidesteps this** — a PDU is a single
  unbroken hex line — which is the main reason PDU is the default here, on top of
  it being the only reliable way to get sender + timestamp + non-ASCII (UCS2) and
  concatenation headers on NB-IoT firmware.

So SMS gets its **own stateful unit, `SmsReceiver`**, rather than bolting state
onto the stateless router. `dispatch_at_line` stays as-is for the status polls;
`on_at_line` feeds SMS-shaped lines to `SmsReceiver` in parallel.

## 3. `SmsReceiver` — the pure URC state machine

New `inc/sms_receiver.hpp` + `src/sms_receiver.cpp`. It consumes AT lines and
returns (a) any AT commands the daemon should now send and (b) any fully-decoded
messages. **No I/O, no ACE** — the daemon wires `.commands` to
`m_at->write_line` and `.messages` to `CellularState`.

```cpp
struct SmsMessage {
    std::string sender;   // originating address, +E.164
    std::string text;     // decoded UTF-8 body (from GSM-7 or UCS2)
    std::string scts;     // service-centre timestamp (raw)
    int         index = -1;   // storage slot (for delete), -1 if direct-deliver
    // multipart info (0/0 for a single-part message)
    int ref = 0, part = 0, total = 0;
};

class SmsReceiver {
  public:
    struct Out {
        std::vector<std::string> commands;   // e.g. {"AT+CMGR=3"} then {"AT+CMGD=3"}
        std::vector<SmsMessage>  messages;    // completed (reassembled) messages
    };
    /// Feed one AT line. Recognises +CMTI / +CMGR / +CMGL / +CMT and the PDU
    /// line that follows a header. Returns commands to issue + finished messages.
    Out on_line(const std::string& line);
    bool interested(const std::string& line) const;  // cheap prefilter for on_at_line
};
```

State machine (PDU mode):

| In state | On line | Action | Next state |
| --- | --- | --- | --- |
| Idle | `+CMTI: "SM",<i>` (URC) | emit `AT+CMGR=<i>`; remember `i` | AwaitCmgrHdr |
| Idle | `+CMGL: <i>,<stat>,…` (drain reply) | remember `i` | AwaitListPdu |
| Idle | `+CMT: …` (direct-deliver, CNMI mode 2,2) | — | AwaitDeliverPdu |
| AwaitCmgrHdr | `+CMGR: <stat>,<alpha>,<len>` | — | AwaitCmgrPdu |
| AwaitCmgrPdu | `<hex PDU>` | decode; if complete emit `AT+CMGD=<i>` + push msg | Idle |
| AwaitListPdu | `<hex PDU>` | decode + push; (delete deferred to end of drain) | Idle |
| AwaitDeliverPdu | `<hex PDU>` | decode + push | Idle |
| any | `OK` / `ERROR` | reset to Idle (resync guard) | Idle |

Multipart: a decoded part with a UDH concatenation IE (`ref/part/total`) goes
into `SmsReassembler` (keyed by `(sender, ref, total)`); a `SmsMessage` is only
pushed to `.messages` once all `total` parts arrive (or a bounded timeout/size
cap drops a stuck partial — logged, never unbounded).

## 4. `sms_pdu` — GSM 03.40 SMS-DELIVER decoder

New `inc/sms_pdu.hpp` + `src/sms_pdu.cpp`, mirroring `at_parser` (free functions,
pure). Decodes a **hex TPDU string** (SMS-DELIVER, the MT direction) into
`SmsMessage`:

- Skip the SMSC header (length-prefixed), read `TP-MTI`, `TP-OA` (semi-octet
  swapped originating address + type-of-address → `+E.164`), `TP-PID`, `TP-DCS`,
  `TP-SCTS` (7-octet timestamp), `TP-UDL`, `TP-UD`.
- **DCS-driven body decode:** `0x00` class → **GSM 7-bit** default alphabet
  (unpack 7→8 bits, map to Unicode incl. the escape table) → UTF-8; `0x08` →
  **UCS2/UTF-16BE** → UTF-8; 8-bit → raw/hex.
- **UDH parse** when `TP-UDHI` set: extract the concatenation IE
  (`0x00` 8-bit ref or `0x08` 16-bit ref → `ref/part/total`) and offset the
  user-data start; feeds `SmsReceiver`'s reassembly.

All inputs are strings; every branch (7-bit with fill bits, UCS2, UDH-offset
septet alignment, multipart) is a table-driven gtest — no modem needed. Known
tricky cases to pin with fixtures: the **7-bit septet fill after a 6-byte UDH**
(alignment), UCS2 surrogate pairs, and empty/max-160-char bodies.

## 5. Task breakdown

| Task | PR | State | Notes |
| --- | --- | --- | --- |
| **A — `sms_pdu` decoder** | A | ✅ **DONE** | `modules/wan/cellular/inc/sms_pdu.hpp` + `src/sms_pdu.cpp`: pure SMS-DELIVER PDU → `SmsMessage` (SMSC skip, semi-octet + alphanumeric TP-OA, SCTS, DCS GSM7/UCS2/8-bit, GSM7 escape table, UDH concat IE, general LSB-first 7-bit unpack incl. UDH fill-bit alignment). **6 gtests over hex fixtures** in `test/sms_pdu_test.cpp` (canonical "How are you?", UCS2, €, alphanumeric sender, concat part, malformed). |
| **B — `SmsReceiver` + reassembler** | B | ✅ **DONE** | `inc/sms_receiver.hpp` + `src/sms_receiver.cpp`: the §3 line→command/message state machine (`wants()` routing, `+CMTI`→`AT+CMGR`→PDU→`AT+CMGD`; `+CMGL` drain w/o mid-listing delete; `+CMT` direct-deliver) + `SmsReassembler` (bounded multipart by (sender,ref,total), order-independent). **7 gtests** in `test/sms_receiver_test.cpp`. Pure — no ACE. |
| **C — schema (`sms.*` keys)** | B | ✅ **DONE** | `schemas/cell.lua` (same `cell` namespace). Read: `sms.enable` (bool, default **false**), `sms.forward.cloud` (bool, default false). Write: `sms.last.sender`, `sms.last.text`, `sms.last.ts`, `sms.count`, `sms.version`. One-namespace / no-key-per-message idiom (`[[feedback_ds_key_reuse]]`). |
| **D — `CellularState` sms fields** | B | ✅ **DONE** | `cell_state.{hpp,cpp}`: `set_sms(const SmsMessage&)` (updates `sms.last.*`, ++`sms.count`, ++`m_smsVersion`) + emits `sms.*` KVs from `to_kv()` only when a message has been seen. Same only-populated-fields + version-counter pattern as `cell.*`/`gps.*`. `CellState.SmsPublished` gtest asserts the batch + count bump. |
| **E — daemon wiring** | C | ✅ **DONE** | `cellular_client.{hpp,cpp}`: (1) one-time setup in `poll_modem` (first poll, gated on `sms.enable`) — `AT+CMGF=0`, `AT+CNMI=2,1,0,0,0`, `AT+CPMS="ME","ME","ME"`, and `AT+CMGL=4` to **drain stored messages**. (2) `on_at_line` routes `m_sms.wants(line)` lines to `SmsReceiver`; `write_line`s each returned command; `m_state.set_sms(msg)` + `publish()` per message. New members `SmsReceiver m_sms; bool m_sms_setup;` + `Config::sms_enable` (read from `sms.enable`). Daemon builds+links in podman. |
| **F — device-ui tile** | C | ✅ **DONE** | `/api/v1/status` (`http-server handler.cpp`) `cell` block gains `sms_sender`/`sms_text`/`sms_ts`/`sms_count` (long-poll watch adds `sms.version`); SPA **WAN → Cellular** page appends "SMS Received / Last SMS From / At / Text" rows (only once `sms_count>0`). Reuses the `observeStatus()` stream + Clarity Property/Value layout (`[[feedback_ui_conventions]]`). `iot-httpd` builds clean in podman; Angular review-only (no local node, `[[reference_ci_builds_on_main_only]]`). |
| **G — Yocto packaging** | C | ✅ **DONE (no change)** | No new package/recipe edit — the new `cellular_core` sources are compiled by the recipe's existing cmake build into `${PN}-cellular`, and `cellular-client.service` already grants `/dev/ttyUSB0..3`. `sms.enable` defaults false, so behaviour is unchanged on boards that don't opt in. |
| **H — HW bring-up** | C | ⬜ TODO (needs HW) | Add an SMS section to `hw-bringup-mangoh-yellow.md`: the by-hand AT recipe (§6 #1), then `sms.enable=true` + restart, send a test SMS to the SIM's MSISDN, confirm `sms.last.*` populates and the tile updates. Sign-off checklist. |

> **Privilege boundary — unchanged.** SMS is read/written over the AT tty the
> `cellular-client` daemon already owns; the lwm2m client stays unprivileged and
> only *reads* `sms.*` from the data-store, exactly as it does `cell.*`/`gps.*`.
> No new capability, no new device node. Per `[[feedback_ace_over_posix]]` all
> new daemon I/O stays ACE (the SMS commands go out through the existing
> `SerialChannel::write_line`).

## 6. Open questions (need hardware / the SIM's operator)

1. **⭐ Does the carrier actually deliver MT SMS on this NB-IoT SIM?** Blocking
   pre-req for everything. By-hand check on the device:
   ```
   # on the RPi, against the WP's AT port
   picocom -b 115200 /dev/ttyUSB2      # or: microcom / cu
   AT+CPIN?            # SIM ready?
   AT+CEREG?           # registered (stat 1/5)?
   AT+CSCA?            # is an SMSC number provisioned? (blank ⇒ MT likely dead)
   AT+CMGF=0           # PDU mode
   AT+CNMI=2,1,0,0,0   # route new-msg as +CMTI URC
   # …now send an SMS to the SIM's MSISDN from a phone…
   # expect: +CMTI: "SM",<index>
   AT+CMGR=<index>     # read it back → +CMGR header + PDU line
   ```
   If no `+CMTI` ever arrives, escalate to the operator (MT-SMS / SMSC
   provisioning) before writing PDU code.
2. **Preferred message storage** — `AT+CPMS?` to see which stores the WP7702
   supports (`"ME"` modem vs `"SM"` SIM); pick in Task E. Affects `AT+CMGD`
   indices and whether storage fills up (we delete after read to avoid a full
   store silently dropping new MT messages).
3. **CNMI mode** — `2,1` (store + `+CMTI`, read via `CMGR`) vs `2,2` (direct
   `+CMT` deliver, no storage). Store+read is the default here (robust across
   firmwares, survives daemon restarts via the `AT+CMGL=4` drain). Confirm the
   WP7702 honours the chosen `<mt>`.
4. **SMSC address** — some SIMs need `AT+CSCA="<number>"` set before MT works;
   check `AT+CSCA?`. If required, add an `sms.smsc` config key (Task C).
5. **Concatenated SMS in the wild** — confirm the operator actually splits long
   messages with a UDH concat IE (vs. sending truncated) so the reassembler is
   exercised on real traffic.

## 7. Explicitly out of scope

- **Sending SMS (MO).** The insertion point is known — `AT+CMGS=<len>` + PDU +
  Ctrl-Z through `SerialChannel::write_line`, gated behind a write-only ds
  command key watched via `data_store::Client::watch` — but it's a separate
  feature (outbound alerting) with its own auth story. Not built here.
- **Acting on an inbound SMS (remote command).** Executing a received message as
  a control command is a security-sensitive follow-up (needs an allow-list +
  authentication of the sender / a shared secret); this TDD only *surfaces* the
  message.
- **Cloud forward** is stubbed as `sms.forward.cloud` (Task C) but the LwM2M
  Send / ds-mirror path is left to a follow-up once receive is HW-proven.

## 8. Build & test

New pure units build/test on the host via podman, exactly like the existing
`cellular_core` suite (`tdd-mangoh-yellow-sensors.md` §5):

```bash
podman run --rm -i -v "$PWD":/src:Z --entrypoint bash ubuntu:22.04 -s <<'SH'
apt-get update -qq && apt-get install -y -qq build-essential cmake libgtest-dev \
    libace-dev liblua5.3-dev zlib1g-dev libssl-dev
# cellular_core with the new sms_pdu + sms_receiver units
cmake -S /src/modules/wan/cellular -B /tmp/c -DCELLULAR_BUILD_TESTS=ON && cmake --build /tmp/c -j4
/tmp/c/test/cellular_test          # existing 17 + new sms_pdu / sms_receiver gtests
# daemon still links against real ACE + datastore_client
cmake -S /src/modules/wan/cellular -B /tmp/d -DCELLULAR_BUILD_DAEMON=ON -DACE_ROOT=/usr && \
    cmake --build /tmp/d -j4 --target cellular-client
SH
```

> CI note (`[[reference_ci_builds_on_main_only]]`): image workflows build on
> `main` only and there's no local C++ compiler on the dev Mac — review each PR's
> C++ in podman before merge. Real MT-SMS numeric/encoding behaviour is only
> provable on hardware (Task H), which is why every decode branch is pinned with
> host PDU fixtures first.

## 9. PR sequence

- **PR-A** ✅ — `sms_pdu` decoder + 6 gtests (pure, no wiring). Zero runtime
  effect.
- **PR-B** ✅ — `SmsReceiver` + `SmsReassembler` + `cell.lua` `sms.*` keys +
  `CellularState::set_sms` + 8 gtests. No daemon behaviour change yet
  (`sms.enable` default false; nothing calls `SmsReceiver` until PR-C).
- **PR-C** ✅ — daemon wiring (one-time setup + `on_at_line` feed) + device-ui
  tile + Yocto (no new package). Behaviour gated off by default; flip
  `sms.enable=true` on the bench for Task H (the on-HW validation).
- **PR-D** ✅ (this session) — cellular diagnostics wired from the HW bring-up
  (§10): multi-RAT registration polling, RAT get/set (`cell.rat`/`cell.rat.current`),
  reject-cause (`cell.reg.reason`), and the `AT+CPMS` storage fix. Daemon + tile +
  config-form. 48/48 gtests, `iot-httpd` clean.

## 10. HW bring-up log — WP7702 on RPi over USB (Task H, 2026-07-04)

First on-hardware bring-up: a **mangOH Yellow (Sierra WP7702)** cabled to an
RPi3B (`iot-fe26a4ff`) by USB. The modem, config and SMS stack all proved
correct: registration, signal, RAT and **MO (send) SMS** were validated on the
real modem. The one thing not yet observed is an **MT (receive) SMS landing on
this particular SIM** — which turned out to be a **global/IoT SIM** (French `+33`
MSISDN, Belgian `+32` SMSC) that registers as a *visitor* on Airtel India, and
whose plan appears to be **MO-capable but not delivering MT** from an external
sender. Every diagnostic finding below is now wired into the daemon + device-ui
so the next bench session is push-button.

### 10.A Discovery + AT access (busybox device)

```sh
lsusb                                   # → 1199:68c0 Sierra Wireless WP7702
ls -l /dev/ttyUSB*                      # qcserial → ttyUSB0=DIAG ttyUSB1=NMEA ttyUSB2=AT
                                        #   (+ qmi_wwan cdc-wdm0 for the QMI data path)
systemctl stop iot-cellular-client      # free the AT port first

# busybox has NO `timeout` and its `stty` rejects combined opts — use read -t:
at(){ exec 3<>/dev/ttyUSB2; printf '%s\r' "$1" >&3; while read -t 2 -r l <&3; do printf '%s\n' "$l"; done; exec 3>&-; echo "--"; }
atl(){ exec 3<>/dev/ttyUSB2; printf '%s\r' "$1" >&3; while read -t 180 -r l <&3; do printf '%s\n' "$l"; done; exec 3>&-; }  # for slow AT+COPS=?
```

### 10.B The diagnostic sequence (and what each gates)

```sh
at "AT"            # OK — port talks
at "ATI"           # WP7702, fw SWI9X06Y_02.32.02.00
at "AT+CPIN?"      # +CPIN: READY   — SIM present/unlocked
at "AT+CSQ"        # +CSQ: 99,99    — ⚠ no signal (see 10.C)
at "AT+CEREG?"     # +CEREG: 0,2    — searching (LTE)
at "AT+CSCA?"      # +CSCA: "+3247…" — ✅ SMSC provisioned (MT-SMS possible)
```

### 10.C Findings (each → a code change in this session)

1. **RAT lock stranded it.** `AT!SELRAT?` → `06, LTE Only`, but `AT+COPS=?`
   returned only `("IND airtel",…,0)` — **2G (GSM) is the sole coverage**. An
   LTE-only modem never leaves "searching". Fix by hand: `AT!ENTERCND="A710"` →
   `AT!SELRAT=02` (GSM) → `AT+CFUN=0/1`; `CSQ` jumped to `12,99` (−89 dBm).
   → **Daemon now applies `cell.rat` via `AT!SELRAT` + a CFUN cycle and publishes
   `cell.rat.current`** (`selrat_index`/`parse_selrat`); device-ui config has a
   RAT dropdown.
2. **Registration lives on `+CREG`, not `+CEREG`, on 2G.** Once on GSM, `AT+CEREG?`
   still read not-registered while `AT+CREG?` → `0,5` (roaming). The daemon polled
   *only* `AT+CEREG?` → it would report a 2G-registered modem as offline.
   → **Daemon now polls `+CREG`/`+CGREG`/`+CEREG` and combines via `best_reg()`.**
3. **`AT+CEER` unsupported** on this firmware (returns `ERROR`).
   → **Daemon queries it best-effort; `parse_ceer` ignores non-`+CEER:` lines,
   publishing `cell.reg.reason` only when the modem actually gives one.**
4. **Storage is `"SM"`, not `"ME"`.** `AT+CPMS?` → `"SM",0,50,…`. My PR-C setup
   forced `AT+CPMS="ME"` which would misdirect reads.
   → **Removed the forced CPMS — the modem's default keeps receive/read stores
   aligned, and `+CMTI` carries the store name + index anyway.**

### 10.D Registration progression

After forcing GSM (10.C #1), registration walked `+CREG: 0,2` (searching) →
`0,3` (denied, transient) → **`0,5` (registered)**. `AT+COPS?` then showed
`IND airtel`, 2G. Note stat **5 = "roaming"** — the tell (with the `+32`/`+33`
identifiers below) that this is a **foreign-homed IoT SIM registering as a
visitor**, not a native Airtel SIM. Once registered, `AT+CFUN` cycles are
avoided (they re-open the search + clear `AT+CNMI`).

### 10.E Finding the SIM's MSISDN (needed to test MT)

`AT+CNUM` returned empty (this SIM doesn't store `EF_MSISDN`). The reliable way:
**send one MO SMS to a known phone — the sender number it shows IS the MSISDN.**

```sh
at "AT+CMGF=1"                             # text mode for the send test
exec 3<>/dev/ttyUSB2
printf 'AT+CMGS="+9198XXXXXXXX"\r' >&3     # a phone you can read
sleep 1; printf 'Hello from WP7702\x1a' >&3   # \x1a = Ctrl-Z ends the message
sleep 6; while read -t 8 -r l <&3; do printf '%s\n' "$l"; done; exec 3>&-
```

→ `+CMGS: <ref>` + OK, and the phone received "Hello from WP7702" **from
`+33 7000023024234`** — a **French M2M number**. So the SIM is a European-homed
IoT SIM (French MSISDN, Belgian SMSC), roaming on Airtel. (USSD `AT+CUSD=1,
"*282#",15` is an alternative, but only meaningful on a native operator SIM.)

### 10.F Outcome — MO ✅, MT pending (SIM entitlement)

| Path | Result |
| --- | --- |
| enumerate / SIM / RAT / signal / operator / SMSC | ✅ |
| **registration** (2G, via `+CREG`) | ✅ `0,5` |
| **MO SMS** (send) | ✅ phone received it from `+33 7000023024234` |
| **MT SMS** (receive) | ❌ never landed — `AT+CPMS?` stayed `"SM",0,50`, `AT+CMGL=4` empty |

`AT+CMGL` reads storage directly (independent of `AT+CNMI`), so an empty inbox
means the **network never delivered** the MT message — a delivery/entitlement
issue, not device config. With MO working, the likely cause is this **M2M SIM's
plan: MT-SMS disabled, restricted to the provider platform, or not enabled for
external/international senders** (common for IoT SIMs). That is a SIM/carrier
setting — the modem and our code are proven, and `sms_pdu`/`SmsReceiver` are
validated by gtests over real GSM 03.40 PDU vectors; they decode the instant a
real `+CMTI`/`+CMGR` arrives.

**To finish Task H:** use a SIM whose plan delivers MT-SMS from an external
sender (a native Airtel/Jio SIM, or the IoT SIM once MT is enabled in the
provider portal). Then either drive it by hand —

```sh
at "AT+CMGF=0"; at "AT+CNMI=2,1,0,0,0"     # (the daemon does this when sms.enable)
# text the SIM's MSISDN, watch for +CMTI: "SM",<n>, then:
at "AT+CMGR=<n>"                           # PDU → decode_sms_deliver
```

— or just set `cell.rat=gsm` (or `auto`) + `sms.enable=true` in the device-ui
**WAN → Cellular** config and start `iot-cellular-client`; the message lands on
the tile's "Last SMS" row.
