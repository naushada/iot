# WP7702 Cellular WAN — Bring-up, eSIM/SIM-slot Trap, and the `wwan0` Question

Field investigation on a **Raspberry Pi 3B + mangOH (Sierra WP7702)** rig,
2026-07-10, device `192.168.1.20`. **QMI/`qmicli` follow-up 2026-07-19** on the
same board after reflashing the latest bundle (§4.2).

Companion to [`hw-bringup-mangoh-yellow.md`](hw-bringup-mangoh-yellow.md) (wiring
and sensor plane) and [`tdd-mangoh-yellow-sensors.md`](tdd-mangoh-yellow-sensors.md)
(the `cell.*` / `gps.*` telemetry design).

---

## TL;DR

Symptom: `wwan0` enumerated but never got an IP; cellular WAN dead.

1. **Root cause of "no cellular data": the board was running its soldered
   eSIM, not the external SIM card.** `AT!UIMS?` returned `1` (embedded).
   The eSIM held a Sierra/TIM profile (APN `iot.swir`) that attached, got an IP,
   and passed **zero downlink bytes**. Fixed persistently with `AT!UIMS=0`.
2. After the slot change the external **Airtel** SIM registers on its **home**
   network, APN `airtelgprs.com` activates, and **data works**.
3. ⭐ **`wwan0` DOES work — SOLVED 2026-07-19 (§4.2/§4.4).** Root cause: the USB
   composition had **both `rmnet0` and `ecm` enabled** (`AT!USBCOMP?` bitmask
   `0008010D`), and with both present the firmware bridges the PDN to **ECM**,
   leaving the QMI/DirectIP `wwan0` side control-only. Removing ECM from the
   composition — `AT!USBCOMP=1,1,0000010D` (`diag,nmea,modem,rmnet0`) + `AT!RESET`
   — forces the PDN onto DirectIP. After that, `qmicli --wds-start-network`
   +`raw_ip=Y`+the QMI IP makes **`wwan0` carry data**: verified by DNS resolving
   `github.com` over `wwan0` and the netdev RX counter matching the modem WDS RX
   (multi-second latency = real 2G/EDGE). The old `AT$QCRMCALL → NO CARRIER` and
   the QMI "connected but deaf" were both symptoms of the ECM bridge.
4. **Tradeoff: it's ECM *or* DirectIP, not both.** Dropping ECM removes `eth1`.
   The **ECM path** (`eth1` ↔ module `ecm0`, module NAT) still works and was the
   only path before this fix — but adopting `wwan0`/DirectIP means reverting the
   repo's `net.iface.cellular.name` from `eth1` back to `wwan0` and automating the
   QMI data-call bring-up (§4.4, §6.1). Not yet shipped — the fix is HW-proven,
   the plumbing is not.
5. ⚠️ **Poking QMI/`raw_ip`/`start-network` re-enumerates the modem and can wedge
   `/dev/ttyUSB2`** — after which `iot-cellular-client` gets `EPERM` on open (as
   root, with `DeviceAllow` correct) and device-ui shows the modem "not
   connected" with all `cell.*` keys empty (§4.3). A **daemon restart clears it**
   once QMI activity stops — no reboot needed.

---

## 1. Topology — what the WP7702 actually is

The WP7702 is **not a bare modem**. It is a full Linux module
(`swi-mdm9x28-wp`, kernel 3.18.140) running **Legato**, which owns the cellular
data session on its own internal `rmnet_data0`. The Pi is a USB *host* attached
to it.

```
  Raspberry Pi 3B (Yocto, iot stack)        WP7702 module (Legato Linux)
  ───────────────────────────────────       ─────────────────────────────
    wwan0   (qmi_wwan, 1-1.2:1.8)  ◀──USB──▶  QMI service   [no host data call]
    /dev/cdc-wdm0 (QMI control)                              [untested: qmicli]
    /dev/ttyUSB0..2 (qcserial; AT = USB2) ◀─▶  AT service
    eth1  192.168.2.3/24 (cdc_ether)  ◀──────▶  ecm0 192.168.2.2   ✅ works
                                                 │ NAT
                                                 ▼
                                               rmnet_data0 ── Airtel ── Internet
                                               bridge0 192.168.225.1 (unused)
```

USB identity: `Bus 001 Device 004: ID 1199:68c0 Sierra Wireless WP7702`.
Firmware: `SWI9X06Y_02.32.02.00` (2019/08/30). Carrier config: `SIERRA_001.027_000`.

> `dmesg` prints `config 1 has an invalid interface number: 8 but max is 5` and
> `has no interface number 1/4/5`. This is **normal** for this composition — a
> red herring, not a fault.

**Reaching the module:**

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa root@192.168.2.2       # no password
export PATH=$PATH:/legato/systems/current/bin:/sbin:/usr/sbin   # cm, ip, iptables
```

`cm data` / `cm sim info` / `cm radio` on the module are **ground truth**. The
Pi's AT view is only a window onto the module's internal state.

---

## 2. ⭐ The eSIM trap — and the slot change

### 2.1 How it presents

Everything looks healthy, and nothing works:

| Probe | Value | Reads as |
|---|---|---|
| `AT+CPIN?` | `READY` | SIM fine |
| `AT+CREG?` / `AT+CGREG?` | `0,5` | registered — **but roaming** |
| `AT+COPS?` | `"AIRTEL Sierra Wireless"` | on Airtel |
| `AT+CGACT?` | `1,1` | context active |
| `AT+CGPADDR=1` | `10.24.233.159` | **has an IP** |
| `AT!GSTATUS?` | `GPRS state: GPRS READY` | data ready |
| `rmnet_data0` counters | **RX 0 packets / 0 bytes ever**, TX climbing | ☠️ dead |

The `RX 0 bytes` against a climbing TX is the tell. The bearer attaches, the
network hands out an IP, and not one byte comes back. The module itself could
not ping its own gateway, could not resolve via the carrier's own DNS, and
could not open TCP.

### 2.2 The actual diagnosis

```sh
# On the module:
cm sim info
#   Type:                   EMBEDDED                       ← ⭐
#   ICCID:                  89332401000010666622
#   Home Network Operator:  I TIM
#   EID:                    89033240817620069918070001333927   ← eUICC ⇒ eSIM
#   IMSI:                   222013068804259

# On the Pi, over AT:
AT!UIMS?     →  !UIMS: 1          # 0 = external slot, 1 = embedded
AT!UIMS=?    →  !UIMS: (0-1),3
```

`Type: EMBEDDED` + a present `EID` means the modem is using the **soldered
eUICC**, and the external SIM slot is deselected. IMSI `222013068804259` is
**MCC 222 / MNC 01 = Telecom Italia**, a Sierra global IoT profile. It was
*roaming* on Airtel with APN `iot.swir` and no working data entitlement.

> **Diagnostic rule:** always confirm *which* SIM the modem selected before
> debugging APNs, RATs, or the data path. An `IMSI` whose MCC/MNC does not match
> the operator you expect means you are on the wrong SIM. Airtel India is
> MCC `404`/`405`.

### 2.3 The slot change (persistent)

```sh
AT!ENTERCND="A710"    # unlock Sierra advanced commands
AT!UIMS=0             # 0 = external SIM slot
AT!UIMS?              # → !UIMS: 0
AT+CFUN=0             # radio off
AT+CFUN=1             # radio on — re-reads the SIM
```

`AT!UIMS` is **persistent across reboot**. A `CFUN` cycle is enough; a full
`AT!RESET` is not required.

### 2.4 Before / after

| | Embedded eSIM (before) | External SIM (after) |
|---|---|---|
| `AT!UIMS?` | `1` | `0` |
| `cm sim info` Type | `EMBEDDED` | external slot |
| IMSI | `222013068804259` (MCC 222, TIM) | `404909288741018` (MCC 404/90, **Airtel**) |
| ICCID | `89332401000010666622` | `8991000925010294882` |
| `AT+CREG?` | `0,5` (roaming) | `0,1` (**home**) |
| `AT+COPS?` | `"AIRTEL Sierra Wireless"` | `"airtel"` |
| APN | `iot.swir` | `airtelgprs.com` |
| IP | `10.24.233.x` | `100.75.219.215/28` |
| Gateway | `10.24.233.176` | `100.75.219.216` |
| DNS | `10.140.0.20` / `10.140.1.20` | `117.96.122.74` / `59.144.127.117` |
| Downlink | **RX 0 bytes ever** | RX climbing; DNS + traffic OK |

Modem/SIM identity once on the external SIM (for the record):

| | |
|---|---|
| Model / firmware | `WP7702` / `SWI9X06Y_02.32.02.00` (2019/08/30) |
| IMEI | `352653090190117` (`IMEI SV: 4`, `FSN: 4L931585040510`) |
| MSISDN | `+919096383701` (`AT+CNUM`) |
| Carrier config | `SIERRA_001.027_000` (`AT!IMPREF?`) |
| Serving cell | GSM DCS1800, `+CSQ` 9–13 ⇒ roughly −95…−87 dBm |

---

## 3. APNs

| SIM | APN | Result |
|---|---|---|
| Airtel (external) | **`airtelgprs.com`** | ✅ consumer APN — activates, data flows |
| Airtel (external) | `airteliot.com` | Airtel M2M/IoT APN (for M2M SIMs) |
| Sierra eSIM | `iot.swir` | activates, but no usable data on this profile |

It is **`airtelgprs.com`**, not `airtel.gprs`.

**Why Airtel APNs failed before the slot change.** On the roaming eSIM, both
Airtel APNs were rejected at `CGACT`:

```
AT+CGDCONT=2,"IP","airtelgprs.com"  →  AT+CGACT=1,2  →  +CME ERROR: requested service option not subscribed
AT+CGDCONT=3,"IP","airteliot.com"   →  AT+CGACT=1,3  →  +CME ERROR: requested service option not subscribed
AT+CGDCONT=1,"IP","iot.swir"        →  AT+CGACT=1,1  →  OK
```

`+CME ERROR: requested service option not subscribed` is **3GPP cause #33**: the
APN is not in the subscription. A **roaming SIM authenticates its APN at the
HOME network**, not the visited one — so a visited operator's APN can never
work. An Airtel APN requires an Airtel SIM.

### 3.1 ⚠️ There are TWO APN settings, and Legato's wins

The modem's AT profile and Legato's `dataConnectionService` config are **separate
stores**. Setting only the AT one looks right until Legato reconnects and silently
reverts to its own value:

```sh
# On the Pi (modem NV, AT CID 1):
AT+CGDCONT=1,"IP","airtelgprs.com"

# ...then on the module, after `app restart dataConnectionService`:
cm data   # →  APN: iot.swir      ← Legato overrode it
```

**Set the Legato one, and note it refuses while the profile is connected**
(`Could not set APN … Maybe the profile is connected`) — and stopping DCS does
*not* release it. You must drop the radio:

```sh
# Pi:      AT+CFUN=0
# Module:  cm data apn airtelgprs.com     # only accepted with the radio down
# Pi:      AT+CFUN=1
# Module:  app restart dataConnectionService
```

Verify with `cm data` → `APN: airtelgprs.com`, `Connected: yes`.

Read the current bearer's IP/gateway/DNS with:

```sh
AT+CGPADDR=1      # → +CGPADDR: 1,100.87.86.56
AT+CGCONTRDP=1    # → IP, gateway, DNS1, DNS2
```

> `+CGCONTRDP` may echo a **stale APN string** in its APN field while its
> IP/DNS fields are correct. Trust `cm data` for the APN.
>
> A `CFUN` cycle **drops the data context**. Re-connect afterwards
> (`app restart dataConnectionService`) or the WAN stays down.

---

## 4. `wwan0` — SOLVED via USB-composition change (QMI DirectIP)

With a healthy home-network SIM, an active context, `raw_ip=Y`, and the link up,
the host-side data call is **still refused over AT**:

```
AT$QCRMCALL=1,1  →  NO CARRIER
AT$QCRMCALL?     →  OK              (no sessions)
AT!SCACT=1,1     →  +CME ERROR: no network service
AT!SCACT?        →  !SCACT: 1,0
```

Result: `wwan0` carries only a link-local address.

**Hypotheses tested and rejected:**

- ~~"The bearer is dead, so the call can't start."~~ It fails identically with a
  fully working Airtel bearer.
- ~~"Legato's `dataConnectionService` hogs the single session."~~
  `app stop dataConnectionService` did **not** release it — `cm data` still
  reported `Connected: yes` and `rmnet_data0` stayed up — and the host call still
  failed. Unproven.
- ~~"Dual-stack `IPV4V6` breaks the 2G bearer."~~ Switching context 1 to `"IP"`
  changed nothing.

### 4.2 ⭐ QMI tested 2026-07-19 — control plane works, data plane still goes to ECM

The latest bundle ships `qmicli 1.34.0` + `libqmi-glib.so.5` (verified on both
`192.168.1.20` and `192.168.1.170`). So the §4 recipe could finally be run against
`/dev/cdc-wdm0`. **The QMI control plane works end to end; the data plane does
not reach `wwan0`.**

What worked (control plane):

```sh
qmicli -d /dev/cdc-wdm0 --dms-get-operating-mode        # → online
qmicli -d /dev/cdc-wdm0 --nas-get-serving-system        # → registered, Airtel
                                                        #   MCC 404/MNC 90, home,
                                                        #   CS+PS attached, GSM/EDGE
qmicli -d /dev/cdc-wdm0 -p --wds-start-network="apn=iot.swir,ip-type=4" \
       --client-no-release-cid                          # → Network started, handle OK
qmicli -d /dev/cdc-wdm0 -p --wds-get-packet-service-status   # → connected
qmicli -d /dev/cdc-wdm0 -p --wds-get-current-settings        # → IP 100.115.65.218/30,
                                                             #   gw .217, Airtel DNS, MTU 1500
```

> **APN surprise:** on this board `apn=iot.swir` **started the network**, while
> `apn=airtelgprs.com` returned `CallFailed / call-end-reason generic-no-service
> (verbose 1075)` this session — the opposite of §3's earlier result on the same
> SIM. Registration was Airtel home (not roaming), so this is either an M2M-SIM
> APN-subscription quirk or a transient 2G attach failure. **Needs a clean
> retest**; do not treat `airtelgprs.com` vs `iot.swir` as settled.

What did **not** work (data plane) — the wall:

- Set `raw_ip=Y` (netdev correctly `POINTOPOINT,NOARP,link/none`), assigned the
  QMI-reported IP, added a route, brought `eth1`/ECM **down** to remove
  contention.
- `ping -I wwan0`, DNS, and `wget` bound to the wwan0 source IP: **100% loss.**
- **The modem's WDS RX counter kept climbing (73 → 105 → 124)** across the tests
  while the Pi's `wwan0` **netdev RX stayed flat.** Downlink reaches the radio but
  is never delivered to the QMI/DirectIP host netdev.

Ruled out as the cause:

| Hypothesis | Test | Result |
|---|---|---|
| `rp_filter` drops asymmetric replies | `all=0`, `wwan0=2` + symmetric `/32` route via wwan0 | still 100% loss |
| ECM steals the session while `eth1` is up | brought `eth1` **down** | still 100% loss; modem RX still climbs |
| Module autoconnect owns the PDN | `--wds-get-autoconnect-settings` | `disabled` — not it |
| netdev in 802.3 mode | `raw_ip=Y`, confirmed `NOARP/link/none` | fixed the mode; data still doesn't flow |

**Interim conclusion (later overturned — see §4.4).** The WP7702 firmware
`SWI9X06Y_02.32.02.00` bridges the PDN downlink to its internal `rmnet_data0 →
ecm0` (CDC-ECM) endpoint, **not** to the QMI/DirectIP `wwan0` netdev *while ECM is
in the USB composition*. `qmicli` can *establish and connect* a WDS session
(further than AT's `$QCRMCALL`), but the bearer's data terminates at ECM. Host-side
routing, `raw_ip`, and `rp_filter` cannot move it. The diagnosis was right; the fix
turned out to be the composition change in §4.4.

### 4.4 ⭐⭐ SOLVED — drop ECM from the USB composition (HW-verified 2026-07-19)

`AT!USBCOMP?` revealed the actual cause: **both `rmnet0` (QMI/DirectIP) and `ecm`
were enabled at once.**

```
AT!ENTERCND="A710"
AT!USBCOMP?
#   Interface bitmask: 0008010D (diag,nmea,modem,rmnet0,ecm)   ← ecm + rmnet0 both on
```

Bit values (from `AT!USBCOMP=?`): `DIAG 0x1, NMEA 0x4, MODEM 0x8, RMNET0 0x100,
ECM 0x80000`. With ECM present the firmware routes the PDN to `ecm0`; `rmnet0`/QMI
is left control-only. **Remove ECM** (keep `rmnet0`) and reset:

```
AT!ENTERCND="A710"
AT!USBCOMP=1,1,0000010D     # diag,nmea,modem,rmnet0  — ECM dropped
AT!USBCOMP?                 # verify → Interface bitmask: 0000010D (diag,nmea,modem,rmnet0)
AT!RESET                    # composition changes apply only after a modem reset
```

After the reset **`eth1` is gone** and `wwan0` remains. Bring up DirectIP from the
Pi:

```sh
ip link set wwan0 down
echo Y > /sys/class/net/wwan0/qmi/raw_ip      # write while link is DOWN
ip link set wwan0 up
qmicli -d /dev/cdc-wdm0 -p --wds-start-network="apn=iot.swir,ip-type=4" \
       --client-no-release-cid
IP=$(qmicli -d /dev/cdc-wdm0 -p --wds-get-current-settings | awk '/IPv4 address/{print $3}')
ip addr add "$IP"/30 dev wwan0
ip route add default dev wwan0 metric 700      # last-resort WAN
```

**Verified on HW:** `nslookup github.com 117.96.122.74` resolved to `20.207.73.82`
over `wwan0`, and the `wwan0` **netdev RX counter matched the modem WDS RX** (7 = 7)
— exactly the equality that was missing in §4.2. Latency is multi-second (2G/EDGE),
and ICMP is flaky/deprioritised as always (§9) — trust DNS + RX counters, not ping.

**Gotchas found while proving it:**

- **The first data call re-enumerates the modem once** (dmesg `qmi_wwan …
  unregister`/`register`), which drops the WDS session and resets `raw_ip` to `N`.
  Re-run the bring-up after it settles; it is stable on the second attempt.
- **`--client-no-release-cid` leaks WDS CIDs**, and after several re-enumerations
  the CTL client returns `CID allocation failed: Transaction timed out`. Recover
  with `killall qmi-proxy` (the `-p` proxy respawns fresh), then retry.
- ⚠️ **This inverts §4.1** — with DirectIP the QMI-reported IP **is** `wwan0`'s
  address, so you *do* `ip addr add` it (unlike the ECM composition, where the
  AT-reported IP belonged to `rmnet_data0`). §4.1 applies only to the ECM layout.
- **PS can detach and `start-network` then fails** `CallFailed / generic-unspecified
  / verbose 1075`, with `--nas-get-serving-system` showing `Registration:
  registered` but `PS: detached`. The AT view can disagree (`AT+CGATT?` → `1`) while
  QMI still reports detached. **`AT+CGATT=1` (wait ~10 s) re-syncs it** → QMI then
  shows `PS: attached` and the call starts. A daemon must confirm PS attach before
  `--wds-start-network`, not just registration.
- **The link is fragile on 2G/EDGE**, independent of the composition: latency is
  multi-second and downlink is lossy (a clean run had `netdev RX == modem RX == 7`
  and resolved DNS; minutes later another attempt passed 1 packet). Plus the modem
  **spontaneously re-enumerates roughly every ~12 min** in this ECM-less layout —
  likely the module's Legato `dataConnectionService` reacting to the missing ECM
  it expects. Each re-enum drops the WDS session and PS attach. **This is the core
  reason DirectIP needs a supervising daemon** (re-attach + re-establish on
  re-enum), not just a one-shot bring-up. Note: dropping ECM also **removes the
  `ssh root@192.168.2.2` module-management path** (it went over `ecm0`), so
  module-side (`cm data`, Legato) debugging is no longer reachable from the Pi.

**Plumbing status.** Implemented in the working tree (not yet built — CI builds on
main only — and not yet HW-validated):

- `net.iface.cellular.name` default `eth1 → wwan0` (`modules/net/router/schemas/net.lua`).
- A DirectIP data-call supervisor in `iot-cellular-client`
  (`modules/wan/cellular/`), gated on the new `cell.data.enable` key (OFF by
  default). Each poll tick it verifies the bearer (QMI `connected` **and** the
  netdev still carries the assigned IP — a re-enum drops both) and re-establishes
  otherwise: force PS attach (`AT+CGATT=1` when QMI shows `PS: detached`),
  `raw_ip=Y`, `qmicli --wds-start-network` (`killall qmi-proxy` + retry on the
  CID-alloc timeout), read `--wds-get-current-settings`, then `ip addr replace` +
  `ip route replace default via <gw> dev wwan0 metric 700` (so net-router
  discovers + re-metrics it) + `resolvectl dns`. Publishes `cell.data.*`.
  - The `qmicli` output parsing is isolated into a pure, unit-tested module
    (`inc/qmi_wan.hpp`, `src/qmi_wan.cpp`, `test/qmi_wan_test.cpp`).
  - New keys: `cell.data.enable`, `cell.qmi.dev`, `cell.wan.iface`, and status
    `cell.data.state/ip/gateway/dns` (`schemas/cell.lua`).
  - The unit gains `DeviceAllow=/dev/cdc-wdm0` and `ProtectKernelTunables=no` (the
    `raw_ip` sysfs write needs `/sys` writable).

**Still to do:** build + HW-validate on .20 (set `cell.data.enable=true`); confirm
`ip route ... via <gw>` works on the raw-ip point-to-point link (only `dev wwan0`
was hand-verified); consider making the supervisor re-establish faster than the
30s poll on a detected re-enum; drop `05-iot-cellular-ecm.network` + the module NAT
script (§5), now vestigial under DirectIP. `AT!USBCOMP` is persistent modem NV; the
host-side bring-up is not.

### 4.3 ⚠️ QMI poking wedges `/dev/ttyUSB2` → cellular-client `EPERM`

A real operational hazard surfaced during the QMI test. Running `qmicli`
`--wds-start-network` / toggling `raw_ip` **re-enumerated the modem** (dmesg:
`qmi_wwan … unregister` then `register`, ttyUSB nodes recreated). After that,
`iot-cellular-client` — which drives the modem over the **AT port
`/dev/ttyUSB2`**, not QMI — logged, every 10 s:

```
[cell] open(/dev/ttyUSB2) failed: Operation not permitted
```

This is **`EPERM`, not `EACCES`**, and the daemon runs as **root** with the unit's
`DeviceAllow` already listing `/dev/ttyUSB2` + `char-ttyUSB` (`DevicePolicy=auto`,
`DynamicUser=no`) — so it is neither a permission-bit nor a device-cgroup problem.
It is the USB-serial port left wedged by the re-enumeration. Consequence: the
daemon can't read the modem, **all `cell.*` / `wan.*` ds keys go unset, and
device-ui shows the cellular modem "not connected."**

> **Lesson:** don't run host QMI experiments on a board whose `iot-cellular-client`
> is live and AT-based — they fight over the modem and the re-enumeration wedges
> the AT port. Stop the daemon first (`systemctl stop iot-cellular-client`) if you
> must poke QMI. **Recovery is just `systemctl restart iot-cellular-client` once
> the QMI activity stops** — the daemon reopens the port cleanly (verified; no
> modem reset or reboot needed).

### 4.1 ⚠️ Never `ip addr add` the AT-reported IP onto `wwan0` (ECM composition only)

> This applies to the **ECM composition** (§5). Under the DirectIP composition
> (§4.4) the QMI-reported IP *is* `wwan0`'s address and you assign it deliberately.

`AT+CGPADDR` reports the address of the module's **internal `rmnet_data0`**, not
of `wwan0`. Assigning it to `wwan0` would make the interface satisfy net-router's
active-WAN predicate in `modules/net/router/src/iface_monitor.cpp`
(`present && up && addr`), so it would be **elected as the active WAN and would
silently blackhole all traffic** — strictly worse than leaving it down.

Also note `+CGPADDR` changes **on every context re-activation**, not per read.
Do not cache it.

---

## 5. What works today — the ECM path

The supported way for an external USB host to reach a WP module's bearer.

**On the module** (`ssh root@192.168.2.2`) — enable forwarding + NAT. Its
`FORWARD` policy is `DROP`, so **both directions** are needed:

```sh
echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -o rmnet_data0 -j MASQUERADE
iptables -A FORWARD -i ecm0 -o rmnet_data0 -j ACCEPT
iptables -A FORWARD -i rmnet_data0 -o ecm0 -m state --state RELATED,ESTABLISHED -j ACCEPT
```

**On the Pi:**

```sh
ip route add default via 192.168.2.2 dev eth1 metric 300
# DNS: 117.96.122.74 / 59.144.127.117 (from AT+CGCONTRDP=1)
```

Verified end-to-end: the Pi resolved `github.com` → `20.207.73.82` through the
modem.

> ⚠️ **None of the module-side config survives a module reboot.** It needs a home
> (a Legato startup app, or re-applied by the Pi over SSH at boot).

---

## 6. Implications for the repo

### 6.1 The cellular slot is `eth1`, not `wwan0` — SHIPPED

`net.iface.cellular.name` used to default to `"wwan0"`; it is now `"eth1"`
(`modules/net/router/schemas/net.lua`). net-router's job for that slot is to
install a **route**, not an address:

- `modules/net/router/src/ip_route.cpp` already runs
  `ip route replace default via <gw> dev <name> metric <(i+1)*100>`.
- net-router **never** does `ip link set up` or `ip addr add` — and for this
  design it must not start (see §4.1).
- `packaging/systemd/iot-net-router.service` is `DynamicUser=yes` with
  `AmbientCapabilities=CAP_NET_ADMIN`, which already covers `ip route`. No new
  privilege needed.
- The gateway is the fixed Sierra convention `192.168.2.2` (module `.2`, host `.3`).

Because net-router only *re-metrics an existing* default route, something must
create one first. That is `05-iot-cellular-ecm.network` (new), which sorts before
`10-iot-wired.network` and matches `Driver=cdc_ether`.

**Verified on HW 2026-07-10:** with `wlan0`'s default routes removed, the Pi
resolved `github.com` and fetched `http://example.com` over the 2G bearer.
net-router logs `route: eth1 metric 300 applied` and keeps `net.iface.active=wlan0`.

> ⚠️ Changing `net.iface.cellular.name` requires **`systemctl restart
> iot-net-router`**. The running daemon kept logging `wwan0 … skipped` after the
> ds key changed — the ds watch wakes the poll loop but the iface name list is
> not recomputed.

### 6.2 Pre-existing nft bootstrap bug (found + fixed here)

`build_nft_ruleset()` emitted `flush table inet iot_router` as its first line.
`nft -f` is **atomic**, so on a device where that table does not exist yet (fresh
boot, or anything that clears nft) the flush errors and the **entire ruleset is
rejected** — the table is never created, so it fails again on the next tick,
forever. Observed live: `nft list tables` was **empty**, `net.state=failed`,
`net.rules.applied.count` unset, DNAT/port-forwarding never applied.

Fix: emit the create-if-absent `add table inet iot_router` *before* the flush.
Confirmed by hand — `nft add table inet iot_router` made the very next tick log
`nft ruleset applied (277 bytes)` and flip `net.state` to `steady`.

Recovery on a running device without the fix: `nft add table inet iot_router`.

### 6.3 `cellular-client`: enabled by default, and it publishes `cell.dns` — SHIPPED

- `iot-cellular-client` used to be `disabled`, so an APN typed into the iot-UI
  wrote a ds key that nothing read. It is now auto-enabled **and** listed in
  `90-iot.preset` (without the preset line, first-boot `preset-all` re-disables
  it — see `reference_systemd_preset_gotcha`). To stay safe on modem-less boards
  the unit carries `ConditionPathExistsGlob=/dev/ttyUSB*`, so systemd skips it
  (inactive, not failed) instead of `Restart=on-failure` looping.
- `AT+CGCONTRDP=1` is now issued every poll and `parse_cgcontrdp_dns()` publishes
  the carrier resolvers to **`cell.dns`** (comma-joined, mirroring
  `vpn.assigned.dns`; IPv4 only). `parse_cgpaddr()` carries a bare address and no
  DNS/gateway/mask, so this was the only way to get them.
- The daemon still **never starts a data call** — no `CGACT`, no QMI, no DHCP. It
  provisions the context (`AT+CGDCONT`) and reports status. The comment in
  `packaging/etc-iot/cellular-client.env` claiming it "connects the data context"
  is wrong.

**On DNS:** the device runs **systemd-resolved**, and `/etc/resolv.conf` is a
symlink to its file listing *global fallback* resolvers (1.1.1.1, 8.8.8.8, …).
Those are reachable through the ECM NAT, so a `wlan0` outage does **not** leave the
device unable to resolve — verified: with cellular as the only default route the Pi
fetched `http://example.com`, which required a lookup. Do **not** write
`/etc/resolv.conf`; it is not ours. Feeding `cell.dns` to the link
(`resolvectl dns eth1 …`) is still worthwhile — Airtel blocks `1.1.1.1`, and the
carrier's own resolvers are closer — but it is an optimisation, not a gap.

### 6.4 ⭐ Root cause: the WP7702 drops AT commands sent back-to-back — FIXED

The symptom: every **one-shot** key stayed empty forever…

    cell.apn.current  cell.model  cell.fw  cell.capability
    cell.rat.current  cell.iccid  cell.imei  cell.msisdn

…while every **every-poll** key populated fine (`cell.state`, `cell.operator`,
`cell.tech`, `cell.reg`, `cell.signal.dbm`, `cell.ip`).

It is **not** a parse mismatch. The replies match the parsers exactly, and
`on_at_line` has a branch for each. The cause is that **the WP7702's AT parser
silently discards a command that arrives before the previous one has answered**,
and the discard is *nondeterministic*.

Reproduce — write ten commands into `/dev/ttyUSB2` in one burst:

| Sent | Answered? |
|---|---|
| `AT+CGDCONT=1,...` | ✅ |
| `AT!ENTERCND="A710"` | ❌ |
| `AT!SELRAT?` | ✅ |
| `ATI` | ❌ |
| `AT+CNUM` | ✅ |
| `AT+CGDCONT?` | ✅ |
| `AT+CSQ` | ❌ |
| `AT+COPS?` | ✅ |
| `AT+CREG?` | ❌ |
| `AT+CGPADDR=1` | ❌ |

Three consecutive runs of a 5-command burst answered three *different* subsets
(`CSQ+ATI+CREG`, then `CSQ+COPS+CGPADDR`, then `CSQ+COPS`). One command at a time,
all five answer, every time.

`poll_modem()` wrote its whole batch without waiting for `OK`. The polled commands
survived because they are re-sent every 30 s and eventually get through. The
one-shots — issued exactly once behind the `m_ident_done` / `m_rat_done` latches —
were lost forever.

**Fix:** `cellular_client` now owns a serialized AT command queue (`cmd()` /
`pump_cmdq()` / `cmd_done()`): one command in flight, the next written only after a
terminal response (`OK`, `ERROR`, `+CME ERROR`, `+CMS ERROR`, `NO CARRIER`,
`ABORTED`). A 15 s watchdog advances the queue if a command never answers, and
`poll_modem()` skips a tick if the backlog exceeds 64. **Never call
`m_at->write_line()` directly** — use `cmd()`.

### 6.5 ⚠️ Second bug: `cell.iccid` could never populate — FIXED

`iccid_command(Vendor::Sierra)` issues `AT+ICCID`, and the WP7702 answers with a
**bare** `ICCID: 8991000925010294882` — no leading `+`. `dispatch_at_line` matched
only `+QCCID:` and `+CCID:`, so the line fell through and `cell.iccid` stayed empty
on every WP module, independently of the burst bug. Now matches all four forms.

### 6.6 ⚠️ `cell.rat` bounces the bearer on every daemon start

If `cell.rat` is non-empty, `poll_modem()` issues `AT!SELRAT=<n>` followed by
`AT+CFUN=0` / `AT+CFUN=1`. **A `CFUN` cycle drops the data context**, so the
bearer re-establishes with a *new* IP on every `systemctl restart
iot-cellular-client`. Observed: `cell.ip` walked `100.87.86.56` →
`100.107.236.103` → `100.109.114.151` across restarts.

This device had `cell.rat=auto` left over from an earlier session. Since LTE-M is
unavailable here (§7), that bought nothing and cost a bearer reset each start.
**Leave `cell.rat` empty** unless you are deliberately changing RAT.

### 6.7 ⚠️ MO SMS send: silently hung, and could wedge the modem — FIXED

Sending from the device-ui left `sms.send.status` at **`sending`** forever and no
message arrived. Three separate faults:

1. `start_send()` wrote `AT+CMGF=0` and `AT+CMGS=<len>` back-to-back — the §6.4
   burst bug. Whichever got dropped, the PDU was written into a modem that had
   never issued its `> ` prompt.
2. The PDU data phase was armed by a **fixed 1 s timer started in `start_send()`**,
   racing the write of `AT+CMGS` itself. It is now armed in `pump_cmdq()` at the
   moment `AT+CMGS=` actually goes on the wire. (The `> ` prompt has no CR/LF, so
   `LineAssembler` never surfaces it as a line — it cannot simply be waited for.)
3. **There was no timeout.** A send that never answered left `sms.send.status` at
   `sending` indefinitely, and worse: a modem parked at `> ` treats every following
   command as *message text*, so the whole daemon goes mute. The command watchdog
   now sends `ESC` (0x1B) to cancel the prompt and publishes
   `failed: modem timeout`. `run()` also sends `ESC` at startup so a modem wedged
   by a previous crash recovers.

Also, `+CME ERROR` / `+CMS ERROR` used to overwrite `sms.send.status` even when no
send was in flight (e.g. answering `AT+CEER`), making a successful send look failed.
It is now gated on an in-flight send.

Verified on HW: one command at a time, honouring the prompt, the WP7702 returns
`+CMGS: 0` / `OK` and the SMS is delivered. SMSC is set (`+CSCA: "+919890051914"`),
storage `+CPMS: "SM",0,25`, `+CSMS: 0,1,1,1`.

### 6.8 device-ui — Cellular Modem page

`iot-ui/src/app/wan/cellular-status/cellular-status.component.ts` already renders
every field (State, Operator, Technology, Registration, Signal, RAT, Capability,
APN, IP Address, DNS, SIM ICCID, IMEI, MSISDN, Model, Firmware). They showed `—`
only because §6.4/§6.5 stopped the daemon from ever publishing them; the fixes
fill them. `cell.dns` is a new row, plumbed through `/status`
(`modules/http-server/src/handler.cpp`) and the `CellStatus` type.

> Note the **IP Address** row shows `cell.ip` = the address of the module's
> internal `rmnet_data0`, i.e. the carrier-assigned WAN address. It is *not* an
> address of any interface on the Pi.

### 6.9 Wishlist

The daemon should read `AT!UIMS?` and publish the selected slot + SIM type
(`EMBEDDED` vs external), so the eSIM trap in §2 is a visible UI field rather
than a day of field investigation.

---

## 7. RAT: locked to 2G

```
AT!SELRAT?  →  !SELRAT: 02, GSM 2G Only
AT!SELRAT=? →  00 Automatic | 01 UMTS 3G Only | 02 GSM 2G Only | 05 GSM+UMTS
               06 LTE Only  | 07 GSM,UMTS,LTE | 11 UMTS+LTE    | 12 GSM+LTE
```

(The `=?` list is generic Sierra boilerplate; the WP7702 has no 3G radio.)

The **WP7702 is LTE-M (Cat-M1) / NB-IoT with 2G fallback.** It does **not** do
5G, nor ordinary LTE Cat-1/4. So a consumer 5G SIM will realistically attach on
**2G GPRS**. Changing RAT needs `AT!ENTERCND="A710"` first and a
`AT+CFUN=0`/`AT+CFUN=1` cycle.

**Tested 2026-07-10 — no LTE-M/NB-IoT service on this SIM/location:**

| Setting | Outcome |
|---|---|
| `!SELRAT=00` (Automatic) | re-registered on **GSM/EGPRS** (`+COPS: …,3`), `+CEREG: 0,4` |
| `!SELRAT=06` (LTE Only) | **never attached** for 80 s — `LTE band: No band`, `PS state: Not attached` |
| `!SELRAT=02` (GSM Only) | registers immediately, `+CREG: 0,1` home |

`AT+WS46=?` → `(12,22,25,28,29)`. There is **no `30`/`31`**, so this firmware does
not expose an NB-IoT-only (`E-UTRAN NB-S1`) mode at all.

⇒ Left pinned at `!SELRAT=02` (GSM only) for fast re-attach. **The rig is stuck at
GPRS speeds** until Airtel provisions LTE-M/NB-IoT on the SIM. Re-test with
`!SELRAT=06` if that changes.

---

## 8. Persistence matrix

| Setting | Where | Survives Pi reboot | Survives module reboot |
|---|---|---|---|
| `AT!UIMS=0` (SIM slot) | modem NV | ✅ | ✅ |
| `AT+CGDCONT=1,"IP","airtelgprs.com"` | modem NV | ✅ | ✅ |
| `AT!SELRAT=<n>` | modem NV | ✅ | ✅ |
| `raw_ip=Y`, `ip link set wwan0 up` | Pi sysfs | ❌ | ❌ |
| module `ip_forward=1` + `iptables` rules | module runtime | ✅ | ❌ |
| Pi default route via `192.168.2.2` | Pi runtime | ❌ | n/a |

---

## 9. Diagnostic cookbook

**Send an AT command from the Pi** (busybox has no `picocom`):

```sh
at() { printf "%s\r" "$1" | microcom -s 115200 -t 2000 /dev/ttyUSB2 | tr -d '\r'; }
at "AT+CGDCONT?"
```

Stop `iot-cellular-client` first if it is running — it owns `/dev/ttyUSB2`.

**QMI control-plane probe** (image now ships `qmicli 1.34.0`). Read-only queries
are safe; `--wds-start-network` re-enumerates the modem and wedges the AT port
(§4.3), so stop `iot-cellular-client` first:

```sh
qmicli -d /dev/cdc-wdm0 --dms-get-operating-mode
qmicli -d /dev/cdc-wdm0 --nas-get-serving-system        # registration / operator / RAT
qmicli -d /dev/cdc-wdm0 --wds-get-profile-list=3gpp     # APN profiles
qmicli -d /dev/cdc-wdm0 -p --wds-get-packet-statistics  # ⭐ modem-side TX/RX — honest liveness
```

> The modem-side **WDS RX packet counter** (`--wds-get-packet-statistics`) is the
> QMI equivalent of `rmnet_data0` RX bytes: it climbs even when `wwan0`'s netdev is
> deaf, which is exactly how §4.2 proved the data terminates at ECM, not QMI.

**Ground-truth checks, in order:**

```sh
AT!UIMS?                  # ⭐ which SIM slot — check this FIRST
cm sim info               # Type: EMBEDDED vs external; IMSI MCC/MNC
cm radio                  # operator, RAT, home vs roaming
cm data                   # APN, Connected, IP, gateway, DNS
ifconfig rmnet_data0      # ⭐ RX bytes — the only honest liveness signal
```

**Traps:**

- ⭐ **Never send AT commands back-to-back.** The WP7702 silently drops a command
  that arrives before the previous one has answered, nondeterministically (§6.4).
  Wait for `OK`/`ERROR` between commands — in the daemon, always go through
  `cmd()`, never `m_at->write_line()`.
- **A stranded `> ` prompt makes the modem mute.** If `AT+CMGS` was issued and the
  PDU never followed, the modem treats every subsequent command as message text.
  Send `ESC` (`printf '\x1b' > /dev/ttyUSB2`) to cancel.
- **`ping` proves nothing.** ICMP is dropped on these APNs. Use `rmnet_data0`
  RX byte counters, or `nslookup <host> <carrier-dns>`. (Same red herring as the
  DTLS `cannot add peer` investigation.) `1.1.1.1` is blocked by Airtel — a failed
  TCP test to it means nothing.
- `+CGACT: 1,1` and a `+CGPADDR` IP do **not** mean data works. RX bytes do.
- `AT+CEER` and `AT+CGCLASS?` return `ERROR` on this firmware — no help.
- `+CGPADDR` changes on every context re-activation, not per read. Two different
  reads returning different IPs is **not** a churning bearer.
- ⭐ **`qmicli --wds-start-network` re-enumerates the modem and wedges
  `/dev/ttyUSB2`** — `iot-cellular-client` then fails `open()` with `EPERM` (as
  root, `DeviceAllow` fine) and device-ui shows the modem "not connected" with
  `cell.*` empty (§4.3). Stop the daemon before QMI experiments; recover with a
  modem `CFUN` cycle or reboot.
- Device busybox lacks `timeout`, `head -c`, `nc -z`, `pkill`, `pgrep`.
- `zsh` on the dev Mac has **no `/dev/tcp`** — port probes there silently
  false-negative. Use `nc -z -G 5 host port`.
- `1.1.1.1` is blocked on Airtel — a failed TCP test to it means nothing.

---

## 10. Open questions

1. Should `cellular-client` own the SIM-slot selection (`AT!UIMS`) and expose it
   as a ds key, so the eSIM trap is a UI field rather than a field investigation?
2. Should net-router push `cell.dns` to the link (`resolvectl dns eth1 …`)? Not a
   correctness gap (§6.3), but Airtel blocks `1.1.1.1` — one of resolved's global
   fallbacks — so the carrier's resolvers would be closer and more reliable.

**Answered:**

- ~~Can `wwan0` work at all via QMI?~~ **Yes — SOLVED (§4.4).** The blocker was
  ECM being in the USB composition alongside `rmnet0`; the firmware bridged the PDN
  to ECM. `AT!USBCOMP=1,1,0000010D` + `AT!RESET` drops ECM, and `wwan0`/DirectIP
  then carries data (HW-verified: DNS over `wwan0`, netdev RX = modem RX). Tradeoff:
  loses `eth1`. Plumbing to ship it is still TODO (§4.4).
- ~~Does Airtel provision LTE-M/NB-IoT on this SIM?~~ **No** — `!SELRAT=06` never
  attaches (§7). Stuck at GPRS.
- ~~Where does the module-side NAT config live?~~ `packaging/mangoh/iot-ecm-nat.sh`,
  installed into the module's flash-backed `/etc/init.d` + `rc5.d` (§5).
- ~~Why do the one-shot identity keys never publish?~~ The WP7702 drops
  back-to-back AT commands; the daemon now serializes them (§6.4). `cell.iccid`
  had a second, independent prefix bug (§6.5).
- ~~Who writes `cell.dns` into `resolv.conf`?~~ **Nobody should.** systemd-resolved
  owns that symlink, and its global fallback resolvers already work over the ECM
  NAT (§6.3).
