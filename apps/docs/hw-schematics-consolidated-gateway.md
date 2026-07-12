# Consolidated IoT Gateway — Schematic Package

**Status:** design intent, for schematic capture
**Date:** 2026-07-12
**Companion to:** [`hw-consolidated-gateway-proposal.md`](./hw-consolidated-gateway-proposal.md)

---

## 0. How to read this — and what it is not

This is a **capture-ready design-intent package**: sheet structure, net-level connectivity, component values, protection, decoupling, and the layout constraints that must survive into the PCB. It is what a hardware engineer captures *from*.

It is **not** a verified netlist. Two rules for whoever captures it:

> ⚠️ **Every pin number must be taken from the current datasheet / SoM pinout, not from this document.** Pin *names* here are load-bearing; pin *numbers* are deliberately omitted where I could not verify them, because a plausible-but-wrong pin number is worse than a blank.
>
> ⚠️ **Every value marked `[CALC]` is a starting point that must be recomputed** against the final rail current, ripple and thermal budget.

Design rules that hold across all sheets:

| Rule | Value |
|---|---|
| Digital I/O | 3.3 V, except SoM-specific 1.8 V banks — **check the SoM bank voltage before assigning any net** |
| Decoupling | 100 nF 0402 X7R per supply pin, ≤ 2 mm from the pin, own via to GND; one 4.7–10 µF bulk per rail per device |
| Unused inputs | tied off, never floating |
| Series termination | 22 Ω on all single-ended clocks (RGMII, SDIO) |
| Test points | every rail, plus CAN_H/CAN_L, plus a 0 Ω shunt in the 3.8 V modem feed |
| Net naming | `P<voltage>` for rails (`P3V3`, `P3V8_MODEM`), `<SUBSYS>_<SIG>` for signals (`CELL_PWRKEY`, `WL_EN`) |

---

## 1. Sheet list

| # | Sheet | Contents |
|---|---|---|
| 1 | **Power in + protection** | 9–36 V input, reverse polarity, TVS/load dump, eFuse |
| 2 | **Rails** | 5 V main buck, 3.8 V modem buck, 3.3 V, power sequencing |
| 3 | **SoM carrier** | Verdin/SMARC connector, signal assignment, straps |
| 4 | **Ethernet** | RGMII PHY + MagJack |
| 5 | **WiFi 6** | Module, SDIO/PCIe, antennas |
| 6 | **Cellular** | Module, USB, power, SIM mux, eSIM, antennas |
| 7 | **CAN** | Transceiver, termination, protection |
| 8 | **I²C bus** | Sensors, RTC, secure element, LED driver |
| 9 | **LEDs + button** | 11 LEDs, factory-reset button |
| 10 | **Debug** | USB-C OTG, UART console, JTAG, boot straps |
| 11 | **Layout & stackup** | Not a schematic — constraints that must reach the PCB |

---

## Sheet 1 — Power input + protection

The board must survive a **vehicle** supply (you run OBD-II/CAN), which means load dump, cranking dips and reverse polarity. This sheet is the one that decides whether the product dies in the field.

```
 VIN_CONN (9–36 V)
     │
     ├──[F1: 3 A slow-blow]──┬──────────────────────────┐
     │                       │                          │
     │              ┌────────▼────────┐        ┌────────▼────────┐
     │              │ D1  SMCJ40CA    │        │  Q1  N-FET      │   reverse-polarity
     │              │ (bidir TVS)     │        │  CSD18540Q5B    │   + ideal diode
     │              │  400 W, 40 V    │        │  (source→drain) │
     │              └────────┬────────┘        └────┬───────┬────┘
     │                       │                      │ gate  │
     │                       │              ┌───────▼─────┐ │
     │                       │              │ U1 LM74700-Q1│ │
     │                       │              │ ideal-diode  │ │
     │                       │              │  controller  │ │
     │                       │              └──────────────┘ │
     │                       │                               │
    GND ────────────────────┴───────────────────────────────┴──► VIN_PROT ──► Sheet 2
                                                                   │
                                                        C1 100 µF/63 V electrolytic
                                                        C2 1 µF/100 V X7R  (HF)
```

| Ref | Part | Value / notes |
|---|---|---|
| F1 | fuse | 3 A slow-blow. `[CALC]` from total input current at 9 V worst case (see §2 budget: ≈ 1.6 A at 9 V — 3 A gives headroom without letting a short cook the traces). |
| D1 | **SMCJ40CA** | Bidirectional TVS, 400 W. Clamps to ~64 V. |
| Q1 + U1 | **LM74700-Q1** + N-FET | Ideal-diode reverse-polarity block. **Chosen over a series Schottky** because a Schottky at 1.6 A burns ~0.7 W and needs a heatsink; the FET burns ~30 mW. |
| C1 | 100 µF / 63 V | Bulk. Voltage rating ≥ 2× max VIN. |

> ⚠️ **ISO 7637-2 pulse 5a (load dump, up to 87 V on a 24 V system) is NOT survived by this front end.** The SMCJ40CA clamps at 64 V and a 36 V-rated buck will die. Two options, and you must pick one at review:
> 1. **Declare the board 12 V-only** and fit a 24 V-tolerant front end (this design as-is).
> 2. **Add a load-dump clamp** (e.g. a series FET + zener clamp, or an LT4363-class surge stopper) and raise the buck to a 100 V part. This costs ~$2 and one extra sheet — cheap insurance if the product ever goes in a truck.
>
> This is an open question for the design review, not something to discover in the field.

---

## Sheet 2 — Rails

### Power budget (drives every rail spec)

| Load | Rail | Typ | **Peak** | Notes |
|---|---|---|---|---|
| SoM (i.MX 93 / AM62) | 5 V | 1.5 W | 3.5 W | includes LPDDR4 + eMMC |
| **Cellular module** | **3.8 V** | 1.0 W | **7.6 W** | **2.0 A burst at 3.8 V during LTE TX.** This is the whole reason for a dedicated rail. |
| WiFi 6 module | 3.3 V | 0.8 W | 2.0 W | TX burst |
| Ethernet PHY | 3.3 V + 1.0 V | 0.5 W | 0.7 W | GbE link-up |
| Sensors + RTC + SE + LED driver | 3.3 V | 0.2 W | 0.7 W | LEDs all-on |
| CAN transceiver | 5 V | 0.1 W | 0.5 W | dominant recessive fault |
| **Total** | | **≈ 4.1 W** | **≈ 15 W** | |

Peaks are **not** coincident in practice, but you must assume they are. Size the input stage for **15 W** → at 9 V minimum input that is **1.7 A in**.

### 2.1 Main 5 V buck

```
 VIN_PROT (9–36 V) ──► U2  LM73605-Q1  (36 V, 5 A synchronous buck)
                        ├─ VIN   ──┬── C: 2× 10 µF/50 V X7R + 100 nF
                        ├─ EN    ──┴── enable divider from VIN (UVLO ≈ 8.5 V) [CALC]
                        ├─ RT    ──── 400 kHz  [CALC: trade ripple vs. efficiency]
                        ├─ SW    ──── L1  4.7 µH, Isat ≥ 8 A  [CALC]
                        ├─ FB    ──── R divider → 5.0 V  [CALC]
                        ├─ PG    ──── PWR_GOOD_5V ──► sequencing (2.4)
                        └─ AGND/PGND
                             │
                             └──► P5V0 ──► SoM, CAN xcvr, 3.3 V LDO/buck, 3.8 V buck
                                   C: 2× 47 µF + 4× 10 µF ceramic
```

**Why 5 A and not 3 A:** the 15 W worst case is 3.0 A at 5 V. A 3 A part run at 100 % of rating has no margin for the modem's TX transient and will droop. `[CALC]` the inductor for ≥ 8 A saturation — inductor saturation under the modem burst is the single most common first-spin power failure.

### 2.2 3.8 V modem rail — the critical one

```
 P5V0 ──► U3  LMR33630 (36 V, 3 A)  ──► P3V8_MODEM
                 ├─ FB → 3.8 V  [CALC]
                 └─ SW → L2 2.2 µH, Isat ≥ 5 A  [CALC]

  At the MODULE's VBAT pins (not at the regulator!):
      C10  470 µF  low-ESR electrolytic/polymer   ◄── TX burst reservoir
      C11  100 µF  ceramic
      C12  10 µF + 100 nF + 10 nF ceramic
      + 0 Ω shunt (TP) in series for current profiling
```

> **This is the sheet that kills first-spin boards.** The module pulls **2 A in ~577 µs bursts** at the LTE frame rate. If the bulk capacitance is short, or placed at the regulator instead of at the module's VBAT pins, VBAT sags below the module's brown-out threshold and the modem **resets mid-registration**. The failure looks exactly like a network/SIM problem, and people chase it for weeks.
>
> Non-negotiable: **470 µF within 10 mm of the module VBAT pins**, wide low-impedance copper (≥ 2 mm, or a plane pour), and the regulator sized so VBAT ripple stays under 300 mV at the burst.

### 2.3 3.3 V rail

From the **SoM's PMIC** if it can source the extra ~1.5 A for the WiFi module and I²C devices — **check the SoM datasheet's available external 3.3 V budget first**. If it cannot (likely), add:

```
 P5V0 ──► U4  TPS62913 (3 A low-noise buck) ──► P3V3   [CALC]
```

Prefer a buck over an LDO: an LDO dropping 5 V → 3.3 V at 1.5 A dissipates 2.6 W, which is a thermal problem in a sealed enclosure.

### 2.4 Sequencing

```
 VIN_PROT ─► 5 V (PG) ─► 3.3 V (PG) ─► SoM PMIC (owns its own 1.8/1.1/DDR ramp)
                                          └─► SOM_RESET_OUT# ─┬─► ETH_PHY_RST#
                                                              ├─► WL_EN     (WiFi, after 3.3 V stable)
                                                              └─► CELL_PWRKEY pulse (see Sheet 6 — NOT a level)
```

Rules: **no rail may be present at a device's I/O pins before that device's core rail is up** (latch-up / body-diode conduction). The modem in particular must not see USB or SIM activity before VBAT is stable — hold its interfaces in reset until `PWR_GOOD_3V8`.

---

## Sheet 3 — SoM carrier

The SoM (Toradex Verdin AM62 / Variscite VAR-SOM-MX93) brings the SoC, LPDDR4, eMMC and PMIC. This sheet is the **connector and the signal map**. Everything here is an assignment table, because the pin numbers belong to the SoM vendor's pinout document, not to me.

| Function | Signal group | To | Notes |
|---|---|---|---|
| Ethernet | `RGMII_TXD[0:3]`, `TX_CTL`, `TXC`, `RXD[0:3]`, `RX_CTL`, `RXC`, `MDC`, `MDIO` | Sheet 4 | 22 Ω series on `TXC`/`RXC`. **Length-match within each group.** |
| WiFi | `SDIO1_CLK/CMD/D[0:3]` **or** `PCIE_TX/RX ±`, `REFCLK ±` | Sheet 5 | Pick per module (§5). SDIO needs 22 Ω series + 10 k pull-ups. |
| Cellular | `USB1_D ±` (USB 2.0 HS), `USB1_VBUS_DET` | Sheet 6 | 90 Ω differential. |
| Debug/OTG | `USB0_D ±`, `USB0_ID`, `USB0_VBUS` | Sheet 10 | Must be the SoC's **OTG-capable** port (recovery + gadget). |
| CAN | `CAN0_TX`, `CAN0_RX` | Sheet 7 | Native FlexCAN/MCAN. **No SPI controller.** |
| I²C | `I2C0_SCL`, `I2C0_SDA` | Sheet 8 | 3.3 V, 4.7 k pull-ups on Sheet 8 only. |
| Console | `UART0_TXD`, `UART0_RXD` | Sheet 10 | + `RTS`/`CTS` optional. |
| GPIO out | `GPIO_SIM_SEL` | Sheet 6 | SIM mux select. |
| GPIO out | `GPIO_CELL_PWRKEY`, `GPIO_CELL_RESET`, `GPIO_WDISABLE` | Sheet 6 | Open-drain — see the warning on Sheet 6. |
| GPIO in | `GPIO_CELL_STATUS`, `GPIO_CELL_RI` | Sheet 6 | Ring indicator wakes on MT-SMS. |
| GPIO out | `GPIO_WL_EN`, `GPIO_BT_EN` | Sheet 5 | |
| **GPIO in** | **`GPIO_BUTTON#`** | Sheet 9 | `gpio-keys`, `BTN_0`. |
| GPIO out | `GPIO_LED_EN` | Sheet 9 | LP5024 enable. |
| Straps | `BOOT_SEL[1:0]` | Sheet 10 | eMMC vs USB-download. |
| JTAG | `TCK/TMS/TDI/TDO/TRST#` | Sheet 10 | |
| Power | `P5V0`, `GND` ×many | Sheet 2 | **Every** GND pin on the connector must be connected. |

**Straps:** every SoC boot/config strap must be resolved with a physical 0603 resistor to a rail — never left to an internal pull. Put a DNP resistor on the opposite rail for both options so the board can be re-strapped on the bench.

---

## Sheet 4 — Ethernet (RGMII PHY + MagJack)

```
  SoM RGMII ──[22 Ω on TXC/RXC]──► U5  DP83867IR
                                     ├─ MDC/MDIO ◄── SoM
                                     ├─ RESET#   ◄── SOM_RESET_OUT#  (min 1 ms after 3.3 V)
                                     ├─ INT#/PWDN → SoM GPIO (optional)
                                     ├─ X1/X2  ──── Y1 25 MHz ±50 ppm crystal, C 18 pF [CALC per CL]
                                     ├─ LED_0/LED_1 → MagJack LEDs
                                     ├─ RBIAS ───── 11 kΩ 1 %   ← **exact, do not substitute**
                                     └─ TX/RX ±  ──► J2 MagJack (integrated magnetics)
                                                       ├─ Bob-Smith termination: 4× 75 Ω → common → 1 nF/2 kV → chassis
                                                       └─ shield → chassis GND via 1 MΩ ∥ 1 nF
```

| Ref | Part | Notes |
|---|---|---|
| U5 | **TI DP83867IR** | Industrial GbE. RGMII internal delay mode — **set via strap, and record which**. Getting RGMII clock skew wrong is the #1 Ethernet bring-up failure; the internal-delay mode exists precisely to avoid trace-length delay hacks. |
| Y1 | 25 MHz crystal | ±50 ppm. Load caps `[CALC]` from the crystal's CL. |
| J2 | MagJack | Integrated magnetics — **1 part instead of 3** (RJ45 + magnetics + LEDs). |
| RBIAS | 11 kΩ 1 % | Sets the PHY's internal bias current. A 5 % part here degrades the link. |

**Strap resistors** on the PHY select PHY address, RGMII delay and auto-neg defaults. These share pins with the RGMII data lines, so the straps are **sampled at reset** — use 2.2 kΩ (strong enough to strap, weak enough not to fight the driver).

---

## Sheet 5 — WiFi 6 module

Interface depends on the module — decide before layout, they are not interchangeable:

| Module | Host interface | Notes |
|---|---|---|
| **u-blox MAYA-W2** (NXP IW612) | **SDIO 3.0** (WiFi) + **UART** (BT) | SDIO is simpler to route than PCIe; needs a clean 4-bit bus. |
| Ezurio Sona IF573 (Infineon) | SDIO **or** PCIe | PCIe gives more headroom for a real AP. |

```
  SoM SDIO1 ──[22 Ω series on CLK]──► M1  WiFi module
     CLK/CMD/D0..D3                     ├─ VIO       ◄── P3V3  (**check: some modules are 1.8 V VIO**)
     (10 kΩ pull-ups to VIO on          ├─ VBAT/VDD  ◄── P3V3, 10 µF + 100 nF + 4.7 µF
      CMD and D0..D3)                   ├─ WL_EN     ◄── GPIO_WL_EN   (10 kΩ pull-DOWN — must boot OFF)
                                        ├─ BT_EN     ◄── GPIO_BT_EN   (10 kΩ pull-DOWN)
                                        ├─ WL_SLP_CLK ◄─ 32.768 kHz  ← **required by most modules**
                                        ├─ BT UART   ◄─► SoM UART (RX/TX/RTS/CTS)
                                        ├─ ANT0 ─────► J3  MHF4 → 2.4/5 GHz antenna  (AP)
                                        └─ ANT1 ─────► J4  MHF4 → 2.4/5 GHz antenna  (STA)
```

**Design notes:**

1. **Two antennas, and they are not optional.** DBDC = 5 GHz STA + 2.4 GHz AP concurrently, and that needs both chains. If you fit one antenna you have silently bought a single-radio part.
2. **32.768 kHz sleep clock** — most modules require it and will exhibit bizarre association instability without it. Source it from the SoM PMIC if it exports one, otherwise fit a dedicated oscillator.
3. `WL_EN` / `BT_EN` **must have pull-downs**, so the radios are off until Linux enables them. A radio that TXes before the driver configures a regulatory domain is a compliance problem.
4. **VIO voltage:** some modules are 1.8 V, some 3.3 V. Mismatching this destroys the module. Confirm against the datasheet before assigning the SDIO bank.
5. **Keep a full ground pour under the module** and honour the vendor's antenna keep-out. Copy the vendor reference layout exactly — this is where you do not get to be creative.

---

## Sheet 6 — Cellular + SIM

```
                          ┌─────────────────────────────────────┐
  P3V8_MODEM ────────────►│ VBAT ×N  (ALL VBAT pins connected!) │
   (470 µF at the pins,   │                                     │
    Sheet 2.2)            │   M2  Quectel EG25-G  (LCC)         │
                          │                                     │
  SoM USB1_D± ───────────►│ USB_DP / USB_DM   (90 Ω diff)       │
  GPIO_CELL_PWRKEY ──────►│ PWRKEY   ◄── **pulse, not level**   │
  GPIO_CELL_RESET ───────►│ RESET#                              │
  GPIO_WDISABLE ─────────►│ W_DISABLE#  (airplane mode)         │
  GPIO_CELL_STATUS ◄──────│ STATUS   (open-drain, 10 k pull-up) │
  GPIO_CELL_RI     ◄──────│ RI       (ring indicator → MT-SMS)  │
                          │                                     │
                          │ MAIN_ANT ──► J5 MHF4  (LTE main)    │
                          │ DIV_ANT  ──► J6 MHF4  (LTE Rx div)  │
                          │ GNSS_ANT ──► J7 MHF4  (GNSS)        │
                          │                                     │
                          │ USIM_VDD / CLK / RST / DATA         │
                          └──────────┬──────────────────────────┘
                                     │
                          ┌──────────▼──────────┐
                          │ U6  TS3A27518E      │  2:1 SIM mux
                          │  SIM mux            │  ◄── GPIO_SIM_SEL
                          └───┬─────────────┬───┘
                              │             │
                    ┌─────────▼──┐   ┌──────▼────────┐
                    │ eSIM MFF2  │   │ J8 nano-SIM   │
                    │ (soldered) │   │ push-push tray│
                    └────────────┘   └───────┬───────┘
                                             │
                                    U7  ESD array (IP4234CZ6)
                                    on CLK/RST/DATA/VDD — **required**,
                                    the tray is a user-touchable port
```

**Design notes — these are the ones that bite:**

1. **`PWRKEY` is a pulse, not a level.** The module powers on when PWRKEY is pulled low for ~500 ms and then *released*. Drive it **open-drain** through a small N-FET, never push-pull, and never hold it low — holding it low can hold the module in reset forever. Add a 10 kΩ pull-up to VBAT (not to 3.3 V).
2. **Connect every VBAT pin.** They are paralleled internally for current sharing; connecting a subset causes localised heating and burst-time droop.
3. **`RI` (ring indicator) to a wake-capable GPIO.** This is what lets an MT-SMS wake the SoC without polling — directly relevant to the SMS work.
4. **GNSS antenna bias:** if you use an *active* GNSS antenna, feed it via the module's `VDD_EXT`/`GNSS_ANT` bias through a ferrite + 10 nH inductor; check whether the module supplies the bias itself before adding an external feed.
5. **SIM ESD is mandatory.** The nano-SIM tray is a finger-accessible port straight into the modem. `IP4234CZ6` (or equivalent) on all four SIM lines, placed **at the connector**, not at the module.
6. **`USB_BOOT`** (if the module has it) → strap pads. It is the recovery path for a bricked modem firmware.
7. **SIM mux `[VERIFY]`:** confirm the TS3A27518E's on-resistance and bandwidth against the SIM clock rate (up to 5 MHz) *and* that it passes the SIM's 1.8 V / 3.0 V class levels. If it is marginal, use a purpose-built dual-SIM switch instead.
8. **Footprint choice:** lay down the **Quectel LCC common footprint** (EC2x/EG2x). One PCB then takes EG25-G (global), EC25-E (EU), EC25-AF (US) or EG915U (cost-down) with **no respin**.

---

## Sheet 7 — CAN

```
  SoM CAN0_TX ──► U8  TCAN1051HGV ──┬── CANH ──┬──[60 Ω]──┬── J9 (OBD-II / vehicle)
  SoM CAN0_RX ◄──   (or ISO1042     │          │          │
  GPIO_CAN_S  ──►    if isolated)   │          C 4.7 nF   │
                     ├─ VCC ◄ P5V0  │          │          │
                     ├─ S (silent)  │          GND        │
                     └─ CANL ───────┴──────────┴──[60 Ω]──┘
                                     │
                            U9  PESD1CAN (TVS) on CANH/CANL
                            L3  common-mode choke (optional, EMC)
```

| Item | Value | Notes |
|---|---|---|
| Termination | **split: 2× 60 Ω + 4.7 nF to GND** | Split termination (rather than a single 120 Ω) kills common-mode noise and is essentially free. **Fit it via 0 Ω jumpers** so the board can sit mid-bus without double-terminating. |
| U8 | TCAN1051HGV | 58 V bus-fault protection, ±12 kV ESD. |
| **Isolated option** | **ISO1042** | ⚠️ **Recommended if the OBD-II port shares the vehicle chassis ground.** A ground offset between the vehicle and a mains-powered gateway will otherwise push current through the transceiver and destroy it. Needs an isolated 5 V supply (e.g. a small transformer driver) — one extra sheet, ~$3. **Decide at review.** |
| U9 | PESD1CAN | Load-dump/ESD clamp at the connector. |

`S` (silent/standby) → SoC GPIO, pulled **low** (normal mode) by default.

---

## Sheet 8 — I²C bus

One bus, six devices. Address collisions are checked here.

```
  SoM I2C0_SCL ──┬── 4.7 kΩ ──► P3V3
  SoM I2C0_SDA ──┼── 4.7 kΩ ──► P3V3
                 │
                 ├── U10  BME680/688     0x76   temp / pressure / humidity
                 ├── U11  BMI160/270     0x68   accel + gyro
                 ├── U12  OPT3001        0x44   ambient light
                 ├── U13  RV-3028-C7     0x52   RTC
                 ├── U14  SE050          0x48   secure element   [VERIFY]
                 └── U15  LP5024         0x28   LED driver (ADDR strapped)  [VERIFY]
```

> ⚠️ **`[VERIFY]` the SE050 and LP5024 addresses against their datasheets and strap options before capture.** The other four are confirmed from the existing driver code (`bme680.hpp:24`, `bmi160.hpp:20`, `opt3001.hpp:19`). No collisions in the map above, but a wrong assumption here is a bus that silently never enumerates.

**Pull-ups:** 4.7 kΩ for 400 kHz Standard/Fast mode with this bus capacitance `[CALC — recompute from the actual trace length and device count; if you go to Fast-mode+ at 1 MHz you will need ~1 kΩ]`. **Fit them on this sheet only** — a second set of pull-ups elsewhere on the bus is a classic and maddening bug.

**RTC backup:**

```
  U13 RV-3028-C7
   ├─ VDD    ◄── P3V3
   ├─ VBACKUP ◄─ C20  0.1 F supercap (or a coin cell)
   │            └─ the RV-3028 has an *internal* backup switchover — no external diode
   └─ CLKOUT → (optional) 32.768 kHz to the WiFi module (Sheet 5) — saves a part
```

The RTC's `CLKOUT` can feed the WiFi module's 32.768 kHz sleep clock — **one part does two jobs.** Verify the drive strength and jitter meet the WiFi module's spec before relying on it.

**Secure element:** `SE050` holds the DTLS PSK / VPN key and provides the immutable unique serial that `iot.serial` should derive from. It needs a **reset** line and an optional **ENA** line to a SoC GPIO.

---

## Sheet 9 — LEDs + button

### LEDs — 11 LEDs, 14 channels, one chip, **zero series resistors**

```
  U15  LP5024  (24× constant-current sinks, I²C)
    ├─ VCC ◄── P3V3,  10 µF + 100 nF
    ├─ EN  ◄── GPIO_LED_EN (10 kΩ pull-down)
    ├─ VLED ◄─ P3V3 (LED anodes — common)
    │
    ├─ OUT0/1/2   ──► LED1  SYS    (RGB)
    ├─ OUT3       ──► LED2  ETH    (green)
    ├─ OUT4       ──► LED3  WIFI   (blue)
    ├─ OUT5       ──► LED4  CELL   (amber)
    ├─ OUT6/7/8   ──► LED5  CLOUD  (RGB)
    ├─ OUT9       ──► LED6  AP     (white)
    ├─ OUT10..13  ──► LED7-10  SIGNAL bar (4× green)
    └─ OUT14..23  ──► spare (10 channels)
```

**Why a driver IC instead of GPIOs:** 14 GPIO-driven LEDs need 14 SoC GPIOs **and 14 series resistors**. The LP5024's sinks are **constant-current — no resistors at all**, so this deletes ~14 passives and 13 GPIOs, and it costs about $1.20. It is a straight BOM *reduction*, which is exactly the "minimum component" brief.

The current per channel is set by an internal register, so **LED brightness is matched in software, not by resistor binning** — a real benefit when mixing red/green/blue dice with different forward voltages.

`leds-lp50xx` is **mainline**, so every channel appears as `/sys/class/leds/*`. That is what lets u-boot and the kernel own the SYS LED before userspace exists.

| Ref | Part | Notes |
|---|---|---|
| LED1, LED5 | RGB 0603 side-fire | Common-anode. |
| LED2/3/4/6 | single-colour 0603 | Side-fire into light pipes. |
| LED7-10 | green 0603 | Signal bar. |
| — | light pipes | Front-edge, on 2.54 mm pitch. Mechanical owns this — **agree the pitch before layout**. |

### Button

```
   P3V3
     │
    10 kΩ  R30
     │
     ├────────────────► GPIO_BUTTON#  (SoM, gpio-keys, BTN_0)
     │
     ├── C30 100 nF ── GND      (hardware debounce)
     │
     ├── D30 ESD TVS ─ GND      (finger-accessible → mandatory)
     │
    SW1  ──────────── GND       (momentary tactile, recessed 1 mm)
```

| Item | Value | Why |
|---|---|---|
| R30 | 10 kΩ pull-up | Idle state = high. Button **active-low**. |
| C30 | 100 nF | RC = 1 ms — kills contact bounce before it reaches the SoC. The DT `debounce-interval = <30>` then handles the rest in software. |
| D30 | ESD TVS | It is a user-pressable button on an external surface. |
| SW1 | tactile, recessed | Recessed 1 mm: survives a toolbox, still finger-pressable, no paperclip. |

> ⚠️ **The device tree must declare `linux,code = <BTN_0>`, NOT `KEY_RESTART`.** `KEY_RESTART` makes systemd reboot the instant the button is touched — no hold-to-confirm, no abort window, and an accidental brush reboots a live device. `BTN_0` hands the event to `iot-buttond`, which implements the hold-time ladder (1–5 s reboot / 5–10 s factory reset / **> 10 s abort**).

---

## Sheet 10 — Debug ports

### USB-C (OTG) — three debug ports for the price of one connector

```
  J10  USB-C receptacle
    ├─ VBUS ──┬── D40 ESD (TPD4S014) ──► VBUS_DET → SoM
    │         └── **not sourced** — this port is device-mode only
    ├─ CC1 ──── 5.1 kΩ ──► GND     ◄── Rd: advertises "I am a device/sink"
    ├─ CC2 ──── 5.1 kΩ ──► GND
    ├─ D+/D- ── (both pairs shorted — USB 2.0 only) ──► SoM USB0_D± (90 Ω diff)
    └─ SHIELD ─ chassis GND via 1 MΩ ∥ 1 nF
```

This one connector gives you, with **no extra silicon**:

1. **SoC recovery** — NXP UUU / TI DFU serial-download to reflash a bricked eMMC without opening the enclosure.
2. **`g_serial`** — console over USB-C, so a field tech needs no TTL cable.
3. **`g_ether`** — SSH over USB, which is how you debug a device whose WAN is down. This is the scenario where every other tool is useless.

### UART console header

```
  J11  4-pin 1.25 mm JST-SH
    1 ─ GND
    2 ─ SOM_UART0_TXD  (board → host)
    3 ─ SOM_UART0_RXD  (host → board)
    4 ─ P3V3  (sense only — 100 Ω series, do NOT power the adapter from it)
```

Keep this even though USB-C gives you `g_serial`: **the UART works when Linux is dead.** It is the only way to see u-boot and a kernel panic. Pin-1-is-GND matches the common FTDI cable convention.

### JTAG

```
  J12  Tag-Connect TC2050-IDC-NL footprint  — **no connector populated, $0 BOM, 0 mm height**
    TCK / TMS / TDI / TDO / TRST# / nSRST / P3V3 / GND
    (Fit a 10-pin 1.27 mm Cortex Debug header on DEV boards only — DNP on production.)
```

### Boot straps

```
  BOOT_SEL[1:0] ── 0603 jumper pads, both rails available (fit-one-DNP-one)
       00 = eMMC (normal)     01 = USB download (recovery)
```

---

## Sheet 11 — Layout & stackup (not a schematic, but non-negotiable)

### Stackup — 6 layer

| Layer | Use |
|---|---|
| L1 | Signal + **RF** (antenna feeds, all on top, no vias in the RF path) |
| L2 | **GND** — solid, unbroken. The reference plane for L1 and L3. |
| L3 | Signal (RGMII, SDIO, USB) |
| L4 | Power planes (P5V0, P3V3, P3V8_MODEM) |
| L5 | **GND** — solid |
| L6 | Signal + passives |

**4 layers is not enough.** You have RF (WiFi ×2, LTE ×2, GNSS), USB HS, RGMII and SDIO, and they all need a continuous reference plane. Trying to save two layers here will cost you an EMC re-spin that dwarfs the PCB saving.

### Impedance

| Net class | Target | Notes |
|---|---|---|
| RF (antenna feeds) | **50 Ω single-ended**, CPWG | Straight, short, on L1, **no vias**, ground-stitched every λ/20. |
| USB 2.0 HS | **90 Ω differential** | Length-match within pair ≤ 0.15 mm. |
| RGMII | 50 Ω SE | Length-match within each group (TX, RX) ≤ 0.5 mm. Use the PHY's **internal delay mode** — don't do skew with trace length. |
| SDIO | 50 Ω SE | Match CLK to data ≤ 1 mm. 22 Ω series at the source. |
| PCIe (if used) | 85 Ω differential | AC-coupling caps on TX. |
| CAN | 120 Ω differential | Tightly coupled pair to the connector. |

### Critical placement

1. **470 µF within 10 mm of the modem VBAT pins.** Repeated because it is the failure that costs the most time.
2. **Antenna connectors on the board edge**, U.FL/MHF4 feeds short and straight. Honour every vendor keep-out — **copy the module reference layout verbatim.**
3. **Crystal (25 MHz PHY) hard against the PHY**, guard-ringed, ground pour under it, nothing routed beneath.
4. **Thermal:** the SoM, the 5 V buck and the modem are the three heat sources. Spread them; stitch vias into the inner GND planes; do not put the modem next to the buck inductor.
5. **RF vs. switchers:** keep the buck inductors and their switch nodes **far** from the antenna feeds and the GNSS path. GNSS is the most sensitive receiver on the board (−160 dBm) and a nearby SW node will desensitise it into uselessness.
6. **Split the grounds only if you fit ISO1042** — otherwise one solid ground, no moats.

---

## Open items for the design review

| # | Item | Why it's open |
|---|---|---|
| 1 | **ISO 7637-2 load dump (Sheet 1)** | The current front end survives 12 V, not a 24 V truck load dump. Declare the environment or add the clamp. |
| 2 | **Isolated CAN (Sheet 7)** | ISO1042 + isolated supply, or accept non-isolated TCAN1051? Driven by whether the OBD-II port shares chassis ground. |
| 3 | **WiFi VIO 1.8 V vs 3.3 V (Sheet 5)** | Module-dependent, destroys the part if wrong. Confirm before assigning the SoM bank. |
| 4 | **SoM 3.3 V budget (Sheet 2.3)** | Can the SoM PMIC source ~1.5 A externally, or do we fit U4? |
| 5 | **SE050 / LP5024 I²C addresses (Sheet 8)** | Marked `[VERIFY]` — must be checked against datasheets and straps. |
| 6 | **Cat-4 vs Cat-1bis (Sheet 6)** | Cat-1bis deletes J6 (Rx-div antenna) and shrinks the board. Blocked on the "must cellular carry a full OTA image?" question from the proposal. |
| 7 | **Light-pipe pitch (Sheet 9)** | Mechanical must agree the front-panel pitch before LED placement is fixed. |
| 8 | **All `[CALC]` values** | Inductors, dividers, fuse, pull-ups — recompute against the final power budget. |

---

## What this package does not give you

Stated plainly so nobody mistakes it for more than it is:

- **No pin numbers.** Names only. Every pin must come off the current datasheet / SoM pinout at capture time.
- **No verified netlist.** This is design intent; the netlist is an output of capture, and ERC/DRC is where it gets proven.
- **No simulation.** The buck loop compensation, the VBAT droop under the 2 A modem burst, and the RF matching all want a real analysis before tape-out.
- **No mechanical.** Board outline, mounting holes, connector edge, enclosure and the light-pipe stack are all open.

The right next step is to hand this to a hardware engineer for capture in KiCad/Altium, close the eight open items at a design review, and then let ERC and a signal-integrity pass find what a text document cannot.
