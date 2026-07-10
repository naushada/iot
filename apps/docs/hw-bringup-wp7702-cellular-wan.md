# WP7702 Cellular WAN — Bring-up, eSIM/SIM-slot Trap, and the `wwan0` Question

Field investigation on a **Raspberry Pi 3B + mangOH (Sierra WP7702)** rig,
2026-07-10, device `192.168.1.20`.

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
3. **`wwan0` still does not come up.** Firmware `SWI9X06Y_02.32.02.00` refuses
   the host-side data call (`AT$QCRMCALL=1,1` → `NO CARRIER`). This is
   **unresolved**; the only untested path is QMI via `qmicli`, which is not in
   the image.
4. **What does work today** is the **ECM path** (`eth1` ↔ module `ecm0`) with
   NAT on the module. Verified end-to-end from the Pi.

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

## 4. `wwan0` — still refused (OPEN)

With a healthy home-network SIM, an active context, `raw_ip=Y`, and the link up,
the host-side data call is **still refused**:

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

**Still untested — the only remaining lead.** QMI directly on the control port:

```sh
ip link set wwan0 down
echo Y > /sys/class/net/wwan0/qmi/raw_ip     # must be written while link is DOWN
ip link set wwan0 up
qmicli -d /dev/cdc-wdm0 --wds-start-network="apn=airtelgprs.com,ip-type=4" \
       --client-no-release-cid
qmicli -d /dev/cdc-wdm0 --wds-get-current-settings   # IP / gateway / DNS / MTU
ip addr add <ip>/<prefix> dev wwan0
```

The image ships **no** `qmicli`, `qmi-network`, `uqmi`, `mmcli`, or
ModemManager. **Adding `libqmi` (from `meta-networking`) is required merely to
find out whether `wwan0` can ever work.** Until that test runs, treat `wwan0` as
unsupported on this hardware.

### 4.1 ⚠️ Never `ip addr add` the AT-reported IP onto `wwan0`

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

### 6.2 Gaps in `cellular-client`

- The unit **`iot-cellular-client` is `disabled`** and `cell.apn` is **empty**,
  so an APN typed into the iot-UI today writes a ds key that nothing reads. If
  enabling it, also add it to `90-iot.preset`, or first-boot `preset-all` will
  re-disable it.
- The daemon issues `AT+CGDCONT` and polls status. It **never starts a data
  call** — no `CGACT`, no QMI, no DHCP. The comment in
  `packaging/etc-iot/cellular-client.env` claiming it "connects the data context"
  is wrong.
- There is **no `parse_cgcontrdp()`**. `parse_cgpaddr()` in
  `modules/wan/cellular/src/at_parser.cpp` yields a bare IP with no DNS, gateway,
  netmask, or MTU. Publishing `cell.dns` needs a new `AT+CGCONTRDP=1` query;
  mirror the existing comma-joined `vpn.assigned.dns` convention.
- The daemon should learn `AT!UIMS?` and publish the selected slot + SIM type, so
  the eSIM trap is visible in the UI instead of costing a day.

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

**Ground-truth checks, in order:**

```sh
AT!UIMS?                  # ⭐ which SIM slot — check this FIRST
cm sim info               # Type: EMBEDDED vs external; IMSI MCC/MNC
cm radio                  # operator, RAT, home vs roaming
cm data                   # APN, Connected, IP, gateway, DNS
ifconfig rmnet_data0      # ⭐ RX bytes — the only honest liveness signal
```

**Traps:**

- **`ping` proves nothing.** ICMP is dropped on these APNs. Use `rmnet_data0`
  RX byte counters, or `nslookup <host> <carrier-dns>`. (Same red herring as the
  DTLS `cannot add peer` investigation.)
- `+CGACT: 1,1` and a `+CGPADDR` IP do **not** mean data works. RX bytes do.
- `AT+CEER` and `AT+CGCLASS?` return `ERROR` on this firmware — no help.
- `+CGPADDR` changes on every context re-activation, not per read. Two different
  reads returning different IPs is **not** a churning bearer.
- Device busybox lacks `timeout`, `head -c`, `nc -z`, `pkill`, `pgrep`.
- `zsh` on the dev Mac has **no `/dev/tcp`** — port probes there silently
  false-negative. Use `nc -z -G 5 host port`.
- `1.1.1.1` is blocked on Airtel — a failed TCP test to it means nothing.

---

## 10. Open questions

1. **Can `wwan0` work at all?** Needs `libqmi` in the image, then
   `qmicli --wds-start-network` on `/dev/cdc-wdm0`. Until then, unanswered.
   If QMI is also refused, the ECM path is the *only* option and `wwan0` should
   be documented as unsupported.
2. **Does Airtel provision LTE-M/NB-IoT on this SIM?** Test `AT!SELRAT=00`.
   Determines whether this rig is stuck at GPRS speeds.
3. **Where does the module-side NAT config live** so it survives a module reboot?
4. Should `cellular-client` own the SIM-slot selection (`AT!UIMS`) and expose it
   as a ds key, so the eSIM trap is a UI field rather than a field investigation?
