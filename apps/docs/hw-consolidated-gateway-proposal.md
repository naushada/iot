# Consolidated IoT Gateway — Hardware Design Proposal

**Status:** proposal / for review
**Date:** 2026-07-12
**Supersedes (hardware):** RPi 3B + mangOH Yellow (WP7702) + MCP2515 CAN HAT + USB modem

---

## 1. What we are consolidating

Today's bring-up rig is four boards stacked:

| Today | Parts | Problems |
|---|---|---|
| RPi 3B (BCM2837) | SoC + 1 GB RAM + SD card | ARMv8.0, USB-attached NIC (`smsc95xx`), no CAN, no RTC, SD-card wear, consumer part |
| BCM43430 WiFi | on-SoC | 2.4 GHz 1x1 only, `brcmfmac` power-save bug (we carry two workarounds), STA-only |
| mangOH Yellow + Sierra WP7702 | USB modem + 3 I²C sensors | Cat-M1/NB-IoT (~300 kbps — **too slow to carry an OTA bundle**), refuses host data call on `wwan0`, soldered eSIM hijacks the SIM slot |
| MCP2515 SPI HAT / USB-CAN | CAN for OBD-II | extra board, extra connector, extra driver |

Target: **one board**, ~80 × 55 mm, that does WiFi STA (uplink) + WiFi AP (provisioning) + Cellular + Ethernet + CAN + I²C sensors + GNSS, with real status LEDs, a factory-reset button, and proper debug/RMA support.

### 1.1 This is a product family, not one board

| | **G1 — Gateway** *(this document)* | **G1-R — Regional variants** | **S1 — SMS/sensor node** |
|---|---|---|---|
| **Use case** | Full IoT gateway: vehicle telemetry, containers, VPN, OTA | Same board, different cellular region | Low-end: SMS + light telemetry only |
| **Cellular** | Quectel **EG25-G** (Cat-4, global) | EC25-E (EU) / EC25-AF (US) | Quectel **BG95-M3** (Cat-M1/NB-IoT) |
| **OTA over cellular** | ✅ full A/B image | ✅ | ❌ config/SMS only |
| **WiFi** | 2×2 STA + provisioning AP | ✅ | optional / none |
| **Ethernet / CAN / containers** | ✅ | ✅ | ❌ |
| **PCB** | G1 | **same PCB — populate only** | separate, smaller board |
| **Software** | one image | **one image** | subset |

**G1 → G1-R costs nothing.** The Quectel EC2x/EG2x family shares a **common LCC footprint**, so a regional variant is a *populate option* on the same PCB — no respin, no new layout, no new BSP.

**G1 → S1 is a different board, and should be.** The parts that make G1 capable (Cat-4 modem, 2×2 WiFi, GbE PHY, CAN, a containers-capable SoM) are exactly the parts an SMS-only node does not want. See §3.2.1 — the important constraint is that S1 stays inside the **Quectel** AT dialect, so both SKUs share one cellular code path.

---

## 2. Block diagram

```
              ┌──────────────── 9–36 V DC in (vehicle 12/24 V safe) ────────────────┐
              │  TVS + reverse-polarity ideal diode + ISO 7637-2 load-dump clamp     │
              └───────┬───────────────────────────────┬─────────────────────────────┘
                      │ 5 V @ 3 A (wide-Vin buck)     │ 3.8 V @ 3 A (modem-only buck,
                      │                               │  470 µF bulk — 2 A TX bursts)
        ┌─────────────▼───────────────┐               │
        │  SoM  (i.MX 93 / AM62x)     │               │
        │  2× A55 · 1–2 GB LPDDR4     │               │
        │  8–16 GB eMMC · PMIC        │               │
        │  ARMv8.2 · cgroup v2        │               │
        └──┬───┬───┬───┬───┬───┬───┬──┘               │
           │   │   │   │   │   │   │                  │
    RGMII ─┘   │   │   │   │   │   └── UART0 ──► 4-pin console hdr + USB-C gadget
           │   │   │   │   │   │
     ┌─────▼─┐ │   │   │   │   └── FlexCAN0 ──► CAN transceiver ──► OBD-II / vehicle bus
     │GbE PHY│ │   │   │   │
     └───┬───┘ │   │   │   └── SDIO/PCIe ──► ★ WiFi 6 module (DBDC)
       MagJack │   │   │                        ├─ STA  (5 GHz)  = WAN uplink
         eth0  │   │   │                        └─ AP   (2.4 GHz) = local clients
               │   │   │
   USB 2.0 HS ─┘   │   └── I²C0 ──┬── BME680/688  0x76   (temp/press/hum)
        │          │              ├── BMI160/270  0x68   (accel/gyro)
   ★ Cellular      │              ├── OPT3001     0x44   (light)
     module (LCC)  │              ├── RV-3028-C7  0x52   (★ RTC — kills the TLS
     ├ AT   ttyUSB2│              │                        "cert not yet valid" bug)
     ├ NMEA ttyUSB1│              ├── SE050 / ATECC608B  (★ secure element: PSK +
     ├ DIAG ttyUSB0│              │                        guaranteed-unique serial)
     └ ECM  eth1   │              └── LP5024      0x28   (★ 24-ch LED driver)
        │          │                     └──► 11 status LEDs (§4)
     eSIM (MFF2)   │
     + nano-SIM ───┘ SIM mux (GPIO-selected — no more eSIM/travel-SIM surprises)

     GPIO ──► ★ Factory-reset / reboot button (§5)
     Tag-Connect TC2050 ──► JTAG (§6)
```

★ = new vs. today.

---

## 3. Component selection

### 3.1 WiFi — one radio, STA (WAN uplink) + provisioning AP

> ### ✅ **DECISION (2026-07-12): the AP is for provisioning only.**
> A technician or installer associates a phone/laptop to configure the box. Customer traffic does **not** live behind it.

This decision **removes DBDC (dual-band dual-concurrent) from the requirements**, and that matters more than it sounds:

- DBDC was the **only unverified, risk-carrying assumption in the entire design** — "5 GHz STA + 2.4 GHz AP simultaneously" is asserted by datasheets and is frequently not true in the driver's `nl80211` interface combinations. That risk is now **gone**.
- A single-radio part costs roughly **half** what a DBDC part does.
- The AP now **shares the STA's channel**, and both halve in throughput. For a provisioning AP carrying a config session, this is irrelevant. It would have been unacceptable for customer traffic.

**Still required:** the module must permit **concurrent AP + STA on one channel** — the AP has to stay up while the STA holds the WAN uplink. Nearly every modern part does, but verify it on an eval board before committing:

```sh
iw phy | sed -n '/valid interface combinations/,/^$/p'
# Required:  #{ managed } <= 1, #{ AP } <= 1   (#channels == 1 is now FINE)
```

**Keep 2×2 MIMO on the STA side.** WiFi is a *primary WAN bearer*, not a convenience — the STA's uplink throughput carries the VPN, LwM2M, telemetry and OTA. Two antennas stay.

| Candidate | Silicon | Driver | Why / why not |
|---|---|---|---|
| **u-blox MAYA-W2** *(recommended)* | NXP IW611/IW612 (WiFi 6 + BT + 802.15.4) | `nxpwifi` / NXP BSP | Industrial vendor, 10-yr longevity, published RMA, strong FAE support, pre-certified FCC/CE. Tri-radio gives you Thread/Zigbee for free later. |
| **Ezurio (ex-Laird) Sona IF573** | Infineon CYW55573 (WiFi 6E) | `brcmfmac`/FMAC | Best-in-class Linux BSP + Yocto layer, long-term support contracts, excellent RMA. 6 GHz headroom. Note: same driver family as the BCM43430 we're fighting — verify the power-save bug is gone on eval. |
| **Murata Type 2EL** | Infineon CYW55573 | `brcmfmac` | Smallest footprint of the three. Murata Linux BSP is solid. |
| **AzureWave AW-XB591NF** *(cost-down)* | MediaTek MT7921 | `mt76` (**mainline**) | Single-radio 2×2 — **now sufficient**, given the provisioning-AP decision. Roughly half the price of a DBDC part, and mainline `mt76` means **no vendor blob-driver dependency**, which is a genuine long-term-maintenance win. ⚠️ Consumer-grade: confirm industrial temperature range and a 10-year longevity commitment before designing it in. |
| ~~Qualcomm QCA/WCN~~ | — | `ath11k` | Needs a Qualcomm account and NDA'd firmware. Not worth it at our volume. |

**Recommendation:** **u-blox MAYA-W2** primary, **Ezurio Sona IF573** second-source — chosen for *industrial temperature range, 10-year longevity and RMA*, which the provisioning-AP decision does **not** relax. Take the MT7921 route only if a per-unit cost target forces it and you accept the consumer-grade lifecycle.

All candidates are **pre-certified modules**, which saves roughly $50–150k in FCC/CE intentional-radiator certification versus a bare chip — a premium that dwarfs the per-unit module cost at our volumes.

### 3.2 Cellular — Cat-4, and the WP7702 has to go

> ### ✅ **DECISION (2026-07-12): cellular must carry a full OTA image.**
> Cellular is a first-class WAN bearer. A device on cellular alone must be able to take a complete A/B image update.

**This settles the class: LTE Cat-4. It is not a preference, it is arithmetic.**

| Bearer | Downlink | Time for a ~450 MB A/B rootfs image | Verdict |
|---|---|---|---|
| **WP7702** (Cat-M1/NB-IoT, today) | ~0.3 Mbps | **≈ 3.5 hours** (best case, no retries) | ❌ Will time out. Unusable. |
| Cat-1bis (EG915U) | ~10 Mbps | ≈ 6 min | ⚠️ Workable, but no headroom once the VPN, LwM2M and telemetry share the link |
| **Cat-4 (EG25-G)** | up to 150 Mbps | **≈ 30–60 s** at realistic field rates | ✅ **Selected** |

Real-world cellular is a fraction of the theoretical rate, and the link is simultaneously carrying the OpenVPN tunnel, LwM2M and telemetry. Cat-4 is what gives an OTA the margin to complete on a mediocre cell in a basement — which is precisely where the devices that most need an update will be.

| Candidate | Class | GNSS | Antennas | Notes |
|---|---|---|---|---|
| **Quectel EG25-G** ✅ **SELECTED** | LTE Cat-4, global bands | integrated | 2 (main + Rx-div) + 1 GNSS | **The code already speaks Quectel** — `cell.lua:8-11` auto-detects via `AT+GMI`/`AT+CGMM`. Same 4× ttyUSB + ECM topology as the WP7702, so `modules/wan/cellular` ports with a config change. LCC solder-down: **no connector, no socket, 3 mm shorter stack**. |
| **Telit LE910C4-WWX** | Cat-4 | integrated | 2 + GNSS | **Second source.** Telit's RMA and carrier-certification support is excellent; slightly pricier. |
| ~~Quectel EG915U / Fibocom L610~~ | Cat-1bis | varies | 1 | ❌ **Rejected by the OTA decision.** Would have saved one antenna and one U.FL. |
| ~~Sierra WP7702~~ | Cat-M1/NB-IoT | — | 1 | ❌ **Rejected.** ~3.5 h for one OTA image. |

**Antenna consequence:** Cat-4 keeps the **Rx-diversity antenna** (J6) — 5 U.FL total (2× WiFi, 2× LTE, 1× GNSS). Rx-diversity is not optional decoration: it is worth 3–6 dB on the downlink, which is exactly the margin an OTA needs at the cell edge, and some carrier certifications require it.

**The footprint trick:** Quectel keeps a **common LCC footprint across the EC2x/EG2x family**. Lay down one footprint and you can populate EG25-G (global), EC25-E (EU) or EC25-AF (US/AT&T) — **one PCB, all regions, no respin**. This is the single highest-leverage decision on the board.

### 3.2.1 Low-end SKU — Cat-M1 / NB-IoT for SMS-only devices

There is a real product below this one: a device that only needs **SMS and light telemetry**, never an OTA image, never a VPN tunnel. Cat-M1/NB-IoT is exactly right for it — lower module cost, far lower power, and much better coverage deep inside buildings. This is a **second SKU**, not a compromise to the Cat-4 design.

**Two things to get right, though:**

**1. Use the Quectel BG95-M3, not the Sierra WP7702.**

| | Sierra WP7702 | **Quectel BG95-M3** ✅ |
|---|---|---|
| Class | Cat-M1 / NB-IoT | Cat-M1 / NB-IoT / EGPRS |
| GNSS | separate | integrated |
| Driver code | Sierra path — needs the `AT!ENTERCND` / `AT!UIMS` incantations, and the eSIM-hijack trap | **`cell.lua:8-11` already auto-detects Quectel.** Same AT dialect as the EG25-G. |
| Consequence | We maintain **two** vendor quirk paths forever | **One** vendor, one AT dialect, one set of quirks across both SKUs |

Staying inside the Quectel family for both SKUs means the cellular daemon has **one** vendor path, and we finally get to **delete the Sierra hacks** rather than carry them for the low-end board. That is worth more than any per-unit saving on the module.

If there is existing WP7702 inventory to burn, the code still supports it — but do not design it into a *new* board.

**2. The SMS-only SKU is a different board, and that is fine.**

The parts that make the flagship expensive — Cat-4 modem, 2×2 WiFi, GbE PHY, CAN, containers-capable SoM — are precisely the parts an SMS-only device does not want. Do not try to build one PCB that is both; you will end up with a board that is too expensive for the low end and too compromised for the high end.

| | **Flagship** (this document) | **Low-end SMS SKU** |
|---|---|---|
| Cellular | Quectel EG25-G (Cat-4) | Quectel BG95-M3 (Cat-M1/NB-IoT) |
| OTA over cellular | ✅ full A/B image | ❌ config/SMS only |
| WiFi | 2×2 STA + provisioning AP | optional / none |
| Ethernet, CAN, containers | ✅ | ❌ |
| SoC | i.MX 93 / AM62 SoM | an MCU or a much smaller SoC |
| Power | mains / vehicle | battery-capable (PSM/eDRX) |

⚠️ **One warning from our own field history:** the WP7702 "no MT-SMS while connected" incident (2026-07-11) was a **CS-domain registration wedge** — `CREG` sat in *searching* while the combined `cell.reg` looked healthy, and only a `CFUN 0/1` cycle recovered it. Whatever module the low-end SKU uses, expose **`cell.reg.cs` / `cell.reg.ps` / `cell.reg.eps` separately** (we already do) and give it a modem-restart path. On an SMS-only device, a silently wedged CS domain is not a degradation — it is total product failure, and nothing else on the box will notice.

**SIM:** MFF2 eSIM (soldered) **plus** a nano-SIM push-push tray, selected by a **TI TS3A27518E SIM mux** on a GPIO. This exists specifically because of the WP7702 lesson — the board booted on its soldered eSIM (TIM/`iot.swir`, zero downlink) while we thought it was on the external Airtel SIM, and the fix was an undocumented `AT!ENTERCND` / `AT!UIMS=0` incantation. With a hardware mux, SIM selection is a GPIO and a `cell.sim.slot` ds key. One extra IC, and it deletes an entire class of field mystery.

### 3.3 SoC / SoM

| Candidate | Cores | CAN | GbE | Longevity | Notes |
|---|---|---|---|---|---|
| **NXP i.MX 93** *(recommended)* | 2× A55, ARMv8.2 | **2× CAN-FD native** | 2× (one TSN) | NXP Product Longevity 15 yr | Native CAN **deletes the MCP2515 HAT**. Native MAC deletes the USB-attached NIC. Low power (~1 W idle), industrial temp, EdgeLock enclave on-die. |
| **TI AM62x (AM625)** | 4× A53 | CAN-FD native | 2× | TI 10+ yr | Excellent mainline support, `meta-ti` is well maintained. Quad-core helps the container workload. |
| Rockchip RK3568 | 4× A55 | 3× CAN-FD | 2× | weaker | Great value/perf, but longevity + PCN discipline are not industrial-grade. Not recommended for a 10-yr product. |

**Buy a SoM, not a bare SoC — for v1 at least.** A SoM brings RAM, eMMC, PMIC, DDR routing and the power-sequencing already solved and validated, so your carrier can be a 6-layer board you can actually get right on the first spin. Move to solder-down later if volume justifies the NRE.

| SoM vendor | Module | Longevity | RMA / support |
|---|---|---|---|
| **Toradex** *(recommended)* | Verdin iMX8M Plus / **Verdin AM62** | 10–15 yr | Free lifetime support tickets, published RMA, best-in-class Yocto layer (`meta-toradex-*`), Torizon debug tooling |
| **Variscite** | VAR-SOM-MX93 | 15 yr | Strong BSP (`meta-variscite`), responsive RMA, good India/EU distribution |
| **phyTEC** | phyCORE-i.MX93 | 15 yr | German engineering support, `meta-phytec`, industrial-grade PCN process |
| Compulab / TechNexion / SolidRun | various | 10 yr | Cheaper; thinner support |

**RAM/eMMC sizing:** 2 GB RAM (the container runtime brief in `tdd-device-containers.md` says caps are mandatory to avoid OOM on 1 GB — 2 GB removes that whole failure mode), **16 GB eMMC**. The A/B layout in `iot-ab.wks.in` needs 100M boot + 2× 1024M rootfs banks + data ≈ 2.4 GB minimum; 16 GB gives room for a bigger data partition and container images.

### 3.4 The rest of the BOM

| Function | Part | Vendor | Why |
|---|---|---|---|
| Ethernet PHY | **DP83867IR** | TI | Industrial GbE, robust, well-supported. Alt: Microchip KSZ9131RNX. |
| RJ45 | integrated-magnetics MagJack | Halo / Pulse / Bel | 1 part instead of 3. |
| CAN transceiver | **TCAN1051HGV** | TI | 58 V bus-fault protection. Use **ISO1042** (isolated) if the OBD-II port is a galvanic-isolation requirement — recommended for vehicle work. |
| **LED driver** | **LP5024** (24× constant-current, I²C) | TI | One chip drives all 11 LEDs. **No series resistors** (built-in current sinks) → ~20 fewer passives. Mainline kernel driver `leds-lp50xx` exposes them as `/sys/class/leds/*` — see §4. Alt: NXP PCA9955B. |
| **RTC** | **RV-3028-C7** + supercap | Micro Crystal | 40 nA, ±1 ppm, 3.2×1.5 mm, mainline `rtc-rv3028`. **This deletes a real bug:** with no RTC the clock is stale at boot, every TLS handshake fails "certificate is not yet valid", and the OpenVPN tunnel never comes up — the reason `10-iot-wired.network` carries `RequiredForOnline=no` today. |
| **Secure element** | **SE050** / ATECC608B | NXP / Microchip | Stores the DTLS PSK and VPN key in hardware, and provides a **guaranteed-unique immutable serial**. `iot.serial` is load-bearing identity across the whole stack (LwM2M endpoint name, `sha256(serial)` PSK identity, VPN cert CN, MQTT topic root) — right now it comes from `/proc/device-tree/serial-number`, which is a *SoM vendor's* promise, not ours. Own it. |
| Sensors | BME680/**BME688**, BMI160/**BMI270**, OPT3001 | Bosch, TI | Keep the existing three so `modules/sensors` ports unchanged. ⚠️ **BMI160 and OPT3001 are heading NRND** — budget driver work for BMI270 / OPT4001, or do a lifetime buy. |
| Power | LM5164 (wide-Vin buck) + LM74700-Q1 (ideal diode) + SMBJ33A | TI | 9–36 V input survives 12 V and 24 V vehicles and the ISO 7637-2 load dump. |

**Indicative BOM (1k units):** SoM $50–70 · WiFi $14 · Cellular $24 · PHY+MagJack $4 · CAN $1 · LED driver $1.2 · RTC $1 · SE $1.5 · SIM mux $0.8 · power $6 · sensors $8 · connectors/antennas $8 · PCB+assembly $20 → **≈ $140–160**.

---

## 4. LED specification

**No LED code exists in the repo today.** This is a clean-sheet design. It is also the single best field-diagnostics investment you can make: today, diagnosing a device means SSH, and if the WAN is down you cannot SSH.

### 4.1 Physical

11 LEDs, all on one **LP5024** (24 channels, 13 spare), 0603 side-fire into light pipes on the front edge.

| # | LED | Type | Channels |
|---|---|---|---|
| 1 | **SYS** | RGB | 3 |
| 2 | **ETH** | green | 1 |
| 3 | **WiFi** | blue | 1 |
| 4 | **CELL** | amber | 1 |
| 5 | **CLOUD** | RGB | 3 |
| 6 | **AP** | white | 1 |
| 7–10 | **SIGNAL** ×4 | green bar | 4 |

**Why three discrete WAN LEDs instead of one RGB:** you asked for *"WAN UP/DOWN, and which WAN is UP."* A single RGB can only show the **active** bearer. Three discrete LEDs show all three bearers **at once** — active, standby-and-ready, or down — which is what a field tech actually needs when deciding whether to blame the SIM or the WiFi. It costs the same three driver channels.

### 4.2 Pattern vocabulary

| Name | Definition |
|---|---|
| `SOLID` | continuously on |
| `SLOW` | 1 Hz, 50 % duty |
| `FAST` | 4 Hz, 50 % duty |
| `BLIP` | 100 ms flash every 2 s |
| `PULSE` | 0.5 Hz breathing (soft ramp) |
| `OFF` | off |

### 4.3 WAN LEDs — ETH / WiFi / CELL

Each bearer's LED tells you three things independently of the other two:

| Pattern | Meaning | ds condition |
|---|---|---|
| `OFF` | bearer down — no carrier, or no interface | iface absent, or `operstate != UP` |
| `FAST` | carrier up, **but no usable IP** (this is your "I have a link but DHCP/PDP failed" state) | `operstate == UP` and no routable IPv4 |
| `SLOW` | up, has an IP, **standby** — ready to take over but not the active WAN | routable IPv4 present, `net.iface.active != this` |
| `SOLID` | **this is the active WAN** | `net.iface.active == {eth,wifi,cellular}` |

Read the row: *all three off* = total WAN loss. *One solid, two slow* = healthy with two hot spares. *Cell fast-blinking* = modem registered but the PDP context failed — go look at the APN, not the antenna. That last one is a genuine 30-minutes-saved-per-incident diagnosis.

`net.iface.active` and the routable-IPv4 test already exist verbatim in `modules/net/router/src/iface_monitor.hpp` — the LED daemon is a pure consumer of keys we already publish.

### 4.4 SYS (RGB)

| Color / pattern | Meaning | Source |
|---|---|---|
| amber `SOLID` | u-boot / early kernel | u-boot sets the LED directly (pre-userspace) |
| amber `PULSE` | booting — kernel up, services starting | kernel `heartbeat` trigger via device tree |
| **green `SOLID`** | **healthy — all enabled units running** | `iot-ledd` sees no failed units |
| green `BLIP` | healthy, low-power / idle | optional |
| amber `SLOW` | degraded — one or more units failed | `systemctl is-failed` non-empty |
| magenta `PULSE` | **OTA in progress** (download → verify → install) | `ota.state` |
| magenta `FAST` | OTA installed — **reboot pending** | `ota.state == reboot-required` |
| red `SLOW` | fault — a critical unit is crash-looping | e.g. `iot-ds` down |
| red `FAST` | **factory reset armed** (button held ≥ 5 s) | button daemon, §5 |
| red `SOLID` | factory reset **committed** — wiping, do not power off | `iot-factory-reset` running |

The amber-at-u-boot / amber-pulse-in-kernel stages matter: they mean **the LED tells you something before our userspace exists**, so a board that dies in the bootloader is still diagnosable from the front panel.

### 4.5 CLOUD (RGB) — LwM2M + VPN

This is the one that pays for itself. Every wedge in the memory index — `bootstrapping` forever, `dm-connecting` after a cloud restart, the DTLS "cannot add peer" loop — is a distinct value of `iot.conn.state`, and today you can only see it by shelling in.

| Color / pattern | Meaning | ds key |
|---|---|---|
| `OFF` | no WAN — not even trying | `net.state == down` |
| blue `SLOW` | VPN connecting | `vpn.state == connecting` |
| blue `SOLID` | VPN up, LwM2M not yet registered | `vpn.state == up` |
| amber `SLOW` | **bootstrapping** (talking to `/bs`) | `iot.conn.state == bootstrapping` |
| amber `FAST` | **DM connecting** (DTLS handshake to the DM server) | `iot.conn.state == dm-connecting` |
| **green `SOLID`** | **registered + VPN up — fully healthy** | `iot.conn.state == registered` && `vpn.state == up` |
| green `SLOW` | registered, but VPN down | `registered` && `vpn.state != up` |
| red `SLOW` | stuck > 120 s in any connecting state | `iot-ledd` timer |

An amber-fast LED that never turns green **is** the DM re-handshake wedge, visible from across the room.

### 4.6 SIGNAL (4-bar) and AP

- **SIGNAL** mirrors `cell.signal.bars` (0–4, already published by `modules/wan/cellular`) when cellular is the active WAN, and WiFi RSSI (bucketed to 4) when WiFi is. Bars `FAST`-blink as a group while the modem is searching (`cell.reg == searching`) — which is exactly the CS-domain registration wedge from 2026-07-11 made visible.
- **AP**: `OFF` = AP down · `SOLID` = AP up, no clients · `BLIP` = AP up with ≥ 1 associated client. Driven from `wifi.ap.state` / `wifi.ap.clients` (new keys, §7).

### 4.7 How it's driven

`leds-lp50xx` is **mainline**, so all 11 LEDs appear as `/sys/class/leds/*`. That means:

- **u-boot and the kernel** can own SYS before userspace exists (device-tree `default-trigger`, `linux,default-trigger = "heartbeat"`).
- A new **`iot-ledd`** is then a thin ds-watcher: subscribe to `net.iface.active`, `net.ifaces`, `iot.conn.state`, `vpn.state`, `cell.signal.bars`, `cell.reg`, `ota.state`, `wifi.ap.*`; write brightness/pattern to sysfs. ACE reactor with a timer for the blink phases — no threads, no polling loops, consistent with the house rule (never raw POSIX, always ACE).

---

## 5. Factory-reset button

### 5.1 Circuit

Single momentary tactile switch → SoC GPIO, with 10 kΩ pull-up, 100 nF debounce cap, and an ESD TVS. Recessed 1 mm into the enclosure so it survives a toolbox but is still finger-pressable — no paperclip.

Declared in the device tree as **`gpio-keys`** with `debounce-interval = <30>` and — importantly — **`linux,code = <BTN_0>`, not `KEY_RESTART`**. `KEY_RESTART` would make systemd reboot the instant you touch it, with no hold-to-confirm and no chance of an abort window.

### 5.2 Behaviour

Duration is measured on **release**, so the LEDs coach you through it while you hold:

| Hold time | Action | LED feedback while held |
|---|---|---|
| < 1 s | **Identify** — flash all LEDs white 3× (find-my-device in a rack) | white flash on release |
| 1–5 s | **Reboot** | SYS amber `SLOW` from t=1 s |
| 5–10 s | **Factory reset** | SYS red `FAST` from t=5 s |
| > 10 s | **Abort** — no action taken | all LEDs `OFF` from t=10 s, then resume |
| held at power-on | **Recovery / USB-download boot** | SYS magenta `SOLID` (read by u-boot) |

The **> 10 s abort** is not decoration. It is the guard against a switch stuck closed by packaging, a screw, or a vibration mount — without it, a jammed button is a fleet-wide silent wipe. And "held at power-on" gives you an unbrick path that does not require opening the enclosure.

### 5.3 Software seam — nothing existing changes

The button daemon does not reboot or wipe anything itself. It **touches the request file**, and the systemd `.path` units we already ship do the rest:

```
iot-buttond  ──touch──►  /run/iot/reboot.request         ──► iot-reboot.path        ──► systemctl reboot
             ──touch──►  /run/iot/factory-reset.request  ──► iot-factory-reset.path ──► /usr/bin/iot-factory-reset
```

That is the same contract the device-UI's `POST /api/v1/system/factory-reset` already uses, so the button gets the identical, already-tested wipe path (stop `iot-ds` → rm `data_store.lua` → rm `-rf /etc/iot/vpn` → sync → reboot) for free. `iot-buttond` reads the evdev fd through the ACE reactor.

⚠️ **`iot-buttond` and `iot-ledd` must be added to `90-iot.preset`.** Units missing from the preset get reset to *disabled* by the first-boot `preset-all` — this already bit `iot-reboot.path` and `iot-factory-reset.path` once (PR #394).

---

## 6. Debug ports

| Port | Implementation | Cost | Buys you |
|---|---|---|---|
| **Serial console** | UART0 → 4-pin 1.25 mm JST-SH header | ~$0.10 | u-boot + kernel-panic visibility. The one port that works when Linux is dead. |
| **USB-C (OTG)** | SoC USB device-mode | 1 connector | Three things on one port, **zero extra silicon**: (a) **SoC recovery** — i.MX serial-download (UUU) / TI DFU to unbrick a device with a corrupt eMMC; (b) **`g_serial`** → console over USB-C, so a field tech needs no TTL cable; (c) **`g_ether`** → SSH over USB, which is how you debug a box whose WAN is down. |
| **JTAG/SWD** | **Tag-Connect TC2050-IDC-NL** footprint | **$0** — no connector populated | Full halt-mode debug on production units (SEGGER J-Link / Lauterbach) with zero BOM and zero z-height. Populate a 10-pin 1.27 mm header on dev boards only. |
| **Modem DIAG** | modem's `ttyUSB0` (already on the shared USB bus) | $0 | QXDM / QLog RF and protocol traces — how you prove a registration failure is the network's fault, not ours. |
| **WiFi FW dump** | `devcoredump` | $0 | Firmware crash dumps to hand the vendor with an RMA. |
| **Boot-mode strap** | 2× 0603 jumper pads | ~$0 | eMMC vs USB-download select. |
| **Test points** | 4× rail TPs, CAN_H/CAN_L, 0 Ω shunt for current | ~$0 | Power-rail bring-up and current profiling without cutting traces. |

The USB-C-as-three-ports trick is the important one: it means **one connector** gives console, recovery, and network debug, which is what "minimum components" actually looks like in practice.

---

## 7. Vendor summary — longevity, RMA, debug

| Subsystem | Primary | Second source | Longevity | RMA / debug support |
|---|---|---|---|---|
| SoM | **Toradex Verdin AM62** | Variscite VAR-SOM-MX93 | 10–15 yr | Toradex: free lifetime support tickets, published RMA, `meta-toradex-*` Yocto layer. Variscite: 15 yr, responsive RMA, good India/EU distribution. |
| SoC | NXP i.MX 93 | TI AM62x | NXP Longevity 15 yr / TI 10+ yr | PCN + EOL notification programs; direct FAE access at our volume via the distributor. |
| WiFi | **u-blox MAYA-W2** (NXP IW612) | Ezurio Sona IF573 (Infineon) | 10 yr | Both pre-certified (FCC/CE/IC modular), both publish RMA processes and maintain Yocto BSP layers. |
| Cellular | **Quectel EG25-G** | Telit LE910C4 | 10 yr (industrial line) | Quectel: pre-certified + PTCRB/GCF, regional FAE, QXDM-class tooling. Telit: strongest carrier-certification support if you need Verizon/AT&T. |
| PHY / CAN / LED / power | TI | Microchip / NXP | 10+ yr | TI's PCN discipline and sample/eval support are the best in the industry. |
| RTC | Micro Crystal RV-3028-C7 | NXP PCF85063A | 10 yr | Both mainline-supported. |
| Secure element | NXP SE050 | Microchip ATECC608B | 10 yr | Both ship Linux middleware + provisioning tooling. |

**Buy-side rule:** every radio must be a **pre-certified module**, not a bare chip. Modular approval means the FCC/CE intentional-radiator certification is already paid for; a bare-chip design puts $50–150k and ~3 months of chamber time on your critical path. At our volume that trade is not close.

---

## 8. Software work this creates

| # | Work | Size | Notes |
|---|---|---|---|
| 1 | **`iot-ledd`** — new daemon: ds-watch → `/sys/class/leds` | S | ACE reactor + timer. New `led.*` schema. Add to `90-iot.preset`. |
| 2 | **`iot-buttond`** — evdev on the ACE reactor → touch `/run/iot/*.request` | S | Zero change to the existing `.path`/`.service` units. Add to `90-iot.preset`. |
| 3 | **WiFi AP mode** — hostapd | M | `modules/wan/wifi/ap/` is **already a reserved empty directory** (`design.md:265`, FUP-L16d). New `wifi.ap.*` ds keys; net-router already owns `nft`, so it can NAT the AP subnet to whatever `net.iface.active` currently is. |
| 4 | **Cellular: WP7702 → Quectel** | S | `cell.lua` **already auto-detects Quectel** via `AT+GMI`/`AT+CGMM`. Retarget `net.iface.cellular.name` to the new ECM iface and **delete the Sierra `AT!ENTERCND` / `AT!UIMS` hacks** entirely. Also fixes the stale `wwan0` comment in `cellular-client.env:8`. |
| 5 | **Drop `modules/bcm2837`** | S | Sensors already prefer `I2cDevTransport` over `/dev/i2c-N`; the MMIO path is a fallback. Deleting it makes the stack **SoC-independent**, and retires `bcm2837-selftest.service`. |
| 6 | **Drop the MCP2515 HAT** | XS | Native FlexCAN. `iot-can0-up.service` is unchanged — it only ever spoke SocketCAN. |
| 7 | **RTC** | XS | `rtc-rv3028` + `hwclock`. Keep `RequiredForOnline=no`, but the "stale clock → TLS cert not yet valid → no VPN" failure class is **designed out**, not worked around. |
| 8 | **`iot.serial` from the secure element** | S | Today it reads `/proc/device-tree/serial-number` — a SoM vendor's promise. Read the SE's immutable serial instead and own the identity that the LwM2M endpoint name, `sha256(serial)` PSK identity, VPN cert CN and MQTT topic root all hang off. |
| 9 | **RAUC + u-boot** for the new SoC | M | `system.conf` hard-codes `mmcblk0p2`/`p3`; confirm eMMC enumeration on the new SoM. Keep `--no-fstab-update` on **every** wic partition (the `/dev/sda4` self-brick). |
| 10 | **New Yocto machine** | M | Replace `meta-raspberrypi` with `meta-toradex-nxp` (or `meta-ti`). `IMAGE_ROOTFS_EXTRA_SPACE` and the **1024 M A/B bank floor** carry over unchanged. |

Items 4–7 are **net deletions of code** — the consolidation pays down debt, it doesn't just add features.

---

## 9. Decisions and open questions

### ✅ Closed (2026-07-12)

| # | Question | Decision | Consequence |
|---|---|---|---|
| 1 | Must cellular carry a full OTA image? | **YES** | **LTE Cat-4 — Quectel EG25-G.** Cat-1bis and Cat-M1 are rejected. Keeps the Rx-diversity antenna (5 U.FL total). Cat-M1 would need ~3.5 h for one image. |
| 2 | Is the WiFi AP for throughput or provisioning? | **Provisioning only** | **DBDC is no longer required** — this deletes the design's only unverified, risk-carrying assumption. Single-radio parts qualify; AP shares the STA channel. Keep 2×2 on the STA side (WiFi is a primary WAN bearer). |

### ⬜ Still open

3. **Galvanic isolation on CAN?** ISO1042 vs TCAN1051 — driven by whether the OBD-II port shares a ground with the vehicle chassis. A ground offset will destroy a non-isolated transceiver.
4. **ISO 7637-2 load dump** — is a 24 V vehicle (87 V load dump) in scope? The current front end survives 12 V only. Adding the clamp costs ~$2 and one sheet.
5. **Enclosure and mounting** — DIN rail, vehicle bracket, or both? Sets the connector edge and therefore the LED light-pipe placement.
6. **BMI160 / OPT3001 NRND** — port to BMI270 / OPT4001 now, or lifetime-buy and defer?

---

## Appendix A — How this proposal was derived (exploration method)

Every hardware claim above is traced back to code in this repo, not to assumption. This appendix records *how*, so a reviewer can re-run the derivation and challenge it.

**Method:** a read-only fan-out sweep of the repository against an 11-point hardware-requirements checklist (LEDs/GPIO · factory-reset & reboot seam · WAN selection · WiFi · cellular · sensors · CAN · containers · storage/partitioning · debug console & serial identity · SoC coupling). Every finding was required to cite `file:line`; a negative finding ("no LED code exists") had to be stated explicitly rather than left silent, because absences are what create the greenfield work in §4 and §5.

**Tools used:**

| Tool | Used for |
|---|---|
| `Explore` subagent (read-only, no write tools) | The 11-point sweep. Run with an explicit "cite file:line, and state negatives explicitly" contract so it could not quietly omit a subsystem. |
| `Grep` / ripgrep | Keyword sweeps for absence-proofs: `led`, `gpio`, `/sys/class/leds`, `libgpiod`, `pinctrl`, `hostapd`. These are what established that LED and AP support genuinely do not exist. |
| `Glob` | Locating the systemd unit set (`packaging/systemd/*.service`, `yocto/.../iot-*.path`) and the wic/RAUC configs. |
| `Read` | Full reads of the load-bearing files: `iface_monitor.hpp`, `net.lua`, `cell.lua`, `wifi.lua`, `container.lua`, `iot-ab.wks.in`, `system.conf`, `iot-factory-reset`, `rpi_serial.cpp`. |
| Project memory | Prior HW-validated findings that are *not* in the code: the WP7702 eSIM/`AT!UIMS` incident, the `brcmfmac` power-save flap, the no-RTC → "cert not yet valid" chain, the wic `--fstab-update` self-brick, the systemd `90-iot.preset` gotcha. These shaped §3.2, §3.4 and §8 as much as the source did. |

**Load-bearing sources** (the files a reviewer should re-read before disagreeing):

- `modules/net/router/src/iface_monitor.hpp:37-41` — the actual definition of "WAN up" (operstate UP **and** a routable non-link-local IPv4). §4.3's LED table is a direct transcription of this predicate.
- `modules/net/router/schemas/net.lua:50-61` — `net.iface.priority = "eth,wifi,cellular"`, and cellular is slot **`eth1`** (the ECM link), not `wwan0`.
- `modules/wan/wifi/client/docs/design.md:265` — `wifi/ap/` reserved-and-empty. This is why AP mode is an M, not an XS.
- `modules/wan/cellular/schemas/cell.lua:8-11` — Quectel auto-detection **already present**. This is what makes the WP7702 → EG25-G swap an S.
- `yocto/.../iot-factory-reset.path:8` + `iot-factory-reset:19-27` — the `/run/iot/*.request` seam the button hangs off, and the exact wipe sequence it inherits.
- `apps/src/rpi_serial.cpp:51-58` + `apps/src/ds_config.cpp:249,258` — `iot.serial` as identity root, which is the argument for the secure element.
- `yocto/meta-iot/wic/iot-ab.wks.in:38-46` — the 1024 M A/B bank floor that sets the eMMC size.

**Known gaps in the derivation** — stated so they are not mistaken for verified:

- Concurrent STA+AP (DBDC) is asserted from vendor datasheets, **not** measured. The `iw phy` interface-combination check in §3.1 is the acceptance test and must be run on an eval board before the part is committed.
- Cat-1bis GNSS availability varies by SKU; confirm against the exact part number at RFQ.
- `mongod`'s ARMv8.2 requirement is **moot on-device** (the device buffers to SQLite; Mongo runs only in the cloud), so it deliberately does *not* drive SoC selection.

---

## Appendix B — Bring-up and debug toolchain

The tools a bench engineer needs to exercise every port in §6, mapped to what each one proves.

| Layer | Tool | Proves |
|---|---|---|
| Power | bench PSU + electronic load; scope on the 3.8 V modem rail | Rail integrity under the modem's 2 A TX burst — the classic first-spin failure. |
| Boot / brick recovery | **NXP UUU** (i.MX) or **TI DFU/`tiboot`** over USB-C | The board is unbrickable from a corrupt eMMC without opening it. |
| Bootloader | serial console on the 4-pin header, 115200 8N1 | u-boot reaches a prompt; RAUC boot-select (`mmc 0:2` / `0:3`) picks the right bank. |
| SoC halt-mode | **SEGGER J-Link** + Tag-Connect TC2050-IDC-NL | DDR calibration, early-boot hangs, anything before the console lives. |
| Kernel / OS | `dmesg`, `udevadm info`, `devcoredump` | Enumeration order — critically, **which iface is `eth0` vs `eth1`**, since `net.iface.*.name` keys on it. |
| WiFi | **`iw phy`** (interface combinations — the §3.1 gate), `iw dev`, `wpa_cli`, `hostapd_cli` | Concurrent 5 GHz STA + 2.4 GHz AP is real, not marketing. |
| Cellular | `mmcli` / **`qmicli`**, raw AT via `picocom /dev/ttyUSB2`, **QXDM/QLog** on `ttyUSB0` | Registration, PDP context, and — when the operator blames us — a protocol trace that proves otherwise. |
| SIM | `AT+CCID`, `AT+CIMI`, plus the new `cell.sim.slot` GPIO | The board is on the SIM you *think* it is on. (This is the WP7702 eSIM lesson, encoded as a test.) |
| CAN | `ip -d link show can0`, `candump`/`cansend` (can-utils), scope on CAN_H/CAN_L | Bit timing at 500 kbps and termination — a 120 Ω mistake looks like "flaky OBD-II". |
| I²C | `i2cdetect -y 1` | All five I²C devices ACK at their expected addresses (0x76, 0x68, 0x44, 0x52, 0x28) before any driver is blamed. |
| LEDs | `echo 255 > /sys/class/leds/<x>/brightness` | Every channel and light pipe, before `iot-ledd` exists. |
| Button | `evtest /dev/input/eventN` | Debounce is clean and the code is `BTN_0`, not `KEY_RESTART`. |
| Network debug | `g_ether` over USB-C → `ssh` | You can get a shell into a device whose WAN is down — the scenario where every other tool is useless. |
| RF / compliance | pre-scan in a chamber; VNA on the antenna feeds | Return loss on all U.FL paths before the formal (expensive) cert run. |
| Application | `iot-dump <module>`, `ds` CLI, `systemctl is-failed` | The existing in-house tooling still works on the new SoC. |
