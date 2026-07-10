# mangOH Yellow ↔ Raspberry Pi 3B — Hardware Bring-up Checklist

Turnkey steps to connect a **mangOH Yellow** kit to a **Raspberry Pi 3B** and
light up the two telemetry planes built in
[`tdd-mangoh-yellow-sensors.md`](tdd-mangoh-yellow-sensors.md):

1. **I²C sensors** → `iot-sensord` → `iot.sensor.*` → IPSO objects + UI tile.
2. **Cellular WAN + GPS** (WP module over USB) → `cellular-client` →
   `cell.*` / `gps.*` → LwM2M Object 6 + UI tiles.

> The two links are **independent**: I²C is a 3-wire harness for the sensors;
> the cellular/GPS plane is a USB cable to the WP module. You can bring them up
> in either order. **Confirm every mangOH-side pin against the board silkscreen
> / mangOH Yellow User Guide before wiring** — the Pi side below is exact, the
> mangOH side names the signals you must locate.

---

## 0. Bill of connections

| Link | From (Pi 3B) | To (mangOH Yellow) | Wires |
|------|--------------|--------------------|-------|
| Sensors | I²C1 (GPIO2/3) | IoT connector I²C (SDA/SCL/GND) | 3 (SDA, SCL, GND) |
| Cellular + GPS | USB-A host port | WP module micro-USB | 1 USB cable |

You also need: the mangOH Yellow on its own power (do **not** back-power it from
the Pi), a SIM in the WP module (for cellular), and 4 female-female jumper wires.

---

## 1. Wiring diagram — I²C (sensors)

```
   Raspberry Pi 3B — 40-pin header                  mangOH Yellow
   (looking at the board, pin 1 top-left)           IoT expansion connector
                                                     (locate on silkscreen)
        3V3  (1) (2)  5V
   SDA1/GPIO2 (3) (4)  5V        ── SDA ───────────▶  I2C_SDA
   SCL1/GPIO3 (5) (6)  GND  ─┐    ── SCL ───────────▶  I2C_SCL
              (7) (8)        │
        GND   (9) (10)       └──── GND ───────────▶  GND
              ... 40-pin ...

   ── Connect exactly THREE wires: SDA↔SDA, SCL↔SCL, GND↔GND. ──
```

**Rules (read before powering):**
- **Do NOT connect 3V3/5V between the boards.** Both are independently powered;
  cross-powering rails can damage them. Share **GND only** on the power side.
- mangOH I²C is **3.3 V** logic — directly compatible with the Pi's 3.3 V I²C.
  Confirm the mangOH IoT-connector I²C bank is the 3.3 V one (not 1.8 V); if the
  signals are 1.8 V you need a level shifter.
- **Pull-ups:** the Pi has 1.8 kΩ pull-ups on SDA1/SCL1 to 3V3. If the mangOH
  side also pulls up, that's usually fine (parallel), but if the bus is flaky,
  remove one side's pull-ups.
- Keep the jumpers short (< 15 cm) — I²C is not meant for long wires.

> ⚠️ **Topology caveat to verify in Step 3:** the onboard BMI160 / BME680 /
> light sensors may sit on the **WP module's private CF3 I²C bus**, not the IoT
> connector. If `i2cdetect` (Step 3) shows an empty bus, the sensors are not
> reachable from the Pi this way — see *Contingency A* in §9.

---

## 2. Wiring diagram — USB (cellular + GPS)

```
   Raspberry Pi 3B                         mangOH Yellow
   ┌───────────┐                           ┌──────────────────────┐
   │  USB-A     │═══════ USB cable ════════│  WP micro-USB (CF3)   │
   │  host port │                          │  + SIM inserted       │
   └───────────┘                           │  + LTE/GNSS antennas  │
                                           └──────────────────────┘
   The WP enumerates as several /dev/ttyUSB* ports (AT / NMEA / DIAG / modem)
   and a wwan/cdc network interface. No GPIO wiring needed.
```

Attach the **LTE** and **GNSS** antennas to the WP module before powering — GPS
needs sky view; run the first GPS test near a window or outdoors.

---

## 3. Pre-flight — enable I²C on the Pi

```bash
# Enable the ARM I²C controller (creates /dev/i2c-1).
sudo raspi-config nonint do_i2c 0          # 0 = enable
# …or add to /boot/config.txt and reboot:
#   dtparam=i2c_arm=on
#   dtparam=i2c_arm_baudrate=100000        # 100 kHz; raise to 400000 once stable
sudo reboot

# After reboot — the node must exist:
ls -l /dev/i2c-1
#   crw-rw---- 1 root i2c 89, 1 ... /dev/i2c-1      ← group "i2c"
sudo apt-get install -y i2c-tools           # i2cdetect / i2cget (Yocto: i2c-tools)
```

> `iot-sensord` **prefers `/dev/i2c-1`** (unprivileged, group `i2c`) and only
> falls back to `/dev/mem` (`--mmio`) if the node is absent — so getting this
> node present is the whole game (PR #293).

---

## 4. Step 1 — I²C discovery

```bash
sudo i2cdetect -y 1
```

Expected (addresses our drivers use — see `modules/sensors/`):

```
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- --
40: -- -- -- -- 44 -- -- -- -- -- -- -- -- -- -- --   ← 0x44 OPT3001 (light)
60: -- -- -- -- -- -- -- -- 68 -- -- -- -- -- -- --   ← 0x68 BMI160 (IMU)
70: -- -- -- -- -- -- 76 --                            ← 0x76 BME680 (env)
```

| Chip | Default addr | Strap alt | Our driver const |
|------|-------------|-----------|------------------|
| BMI160 (accel/gyro) | **0x68** | 0x69 (SDO high) | `Bmi160::kAddrPrimary` |
| BME680 (T/P/H/gas)  | **0x76** | 0x77 (SDO high) | `Bme680::kAddrPrimary` |
| OPT3001 (light)¹    | **0x44** | 0x45/46/47 (ADDR) | `Opt3001::kAddr` |

¹ The exact light part is unconfirmed; if 0x44 is absent but another address
  in 0x44–0x47 responds, note it and pass `--light=0xNN` to `iot-sensord`.

**Also look for** an unexpected device in **0x70–0x77** (a TCA-class I²C
mux/switch or a GPIO expander). If present, the sensors may be **behind** it —
record the address; a mux shim becomes Task B follow-up (TDD §3 note).

**If `i2cdetect` shows all `--`:** the bus isn't reaching the sensors → see
*Contingency A* (§9). Re-check SDA/SCL not swapped, GND connected, and that you
probed the right bus number (`i2cdetect -l` lists buses).

---

## 5. Step 2 — per-chip identity probe

Confirm each chip answers with the **chip-id our driver checks** (so a green
`iot-sensord` is guaranteed):

```bash
# BMI160 — CHIP_ID register 0x00 must read 0xD1
sudo i2cget -y 1 0x68 0x00
#   0xd1   ✔

# BME680 — CHIP_ID register 0xD0 must read 0x61
sudo i2cget -y 1 0x76 0xd0
#   0x61   ✔

# OPT3001 — Device-ID register 0x7F must read 0x3001 (16-bit, MSB-first).
# i2cget word mode returns LSB-first, so 0x3001 shows as 0x0130:
sudo i2cget -y 1 0x44 0x7f w
#   0x0130 ✔   (== 0x3001 byte-swapped)
```

A mismatched id means a different part at that address — update the driver
address/id before proceeding.

---

## 6. Step 3 — bring up `iot-sensord`

```bash
# One-shot smoke (reads once, publishes, exits):
sudo iot-sensord --once --i2c-dev=/dev/i2c-1
sudo journalctl -t iot-sensord -n 20 --no-pager   # "using /dev/i2c-1 (i2c-dev)…"

# Confirm the ds keys appeared:
ds-cli get iot.sensor.temp        # e.g. "22.81"
ds-cli get iot.sensor.accel       # e.g. "-83,12,4071"   (x,y,z raw)
iot-dump iot-sensord              # dump all iot.sensor.*

# Enable the service (NOT auto-enabled — it needs the board):
sudo systemctl enable --now iot-sensord.service
sudo systemctl status iot-sensord.service
```

**UI:** device-ui → **Sensors → Sensors** tile shows temp/humidity/pressure/lux
+ accel/gyro, refreshing live (the `/status` long-poll wakes on
`iot.sensor.version`).

Once green on `/dev/i2c-1`, **drop the privilege** (PR #293): edit
`iot-sensord.service`, remove `AmbientCapabilities=CAP_SYS_RAWIO` +
`CapabilityBoundingSet` + the `/dev/mem`,`/dev/gpiomem` `DeviceAllow`, leaving
only `DeviceAllow=/dev/i2c-1 rw`. `daemon-reload` + restart and re-verify.

---

## 7. Step 4 — cellular + GPS USB discovery

```bash
# 1. Is the WP enumerated?  (Sierra Wireless = USB vendor 1199)
lsusb | grep -i -E "sierra|1199|quectel"

# 2. Which serial ports did it expose?
ls -l /dev/ttyUSB*
#   typically ttyUSB0..3 — DIAG / NMEA(GPS) / AT / modem(PPP), order varies

# 3. Identify the AT port: send AT, expect OK. Try each ttyUSB until one answers.
sudo apt-get install -y picocom
sudo picocom -b 115200 /dev/ttyUSB2     # type:  AT⏎   → expect: OK
#   (Ctrl-A Ctrl-X to quit picocom)
#   Quick non-interactive probe:
#   (stty -F /dev/ttyUSB2 115200; printf 'AT\r' > /dev/ttyUSB2; timeout 2 cat /dev/ttyUSB2)

# 4. Is a managed modem / wwan iface present?
mmcli -L                  # ModemManager: lists /org/.../Modem/0 if managed
ip link | grep -i wwan    # wwan0 (QMI/MBIM) — net-router already routes this slot
```

**Record:** which `ttyUSB` is the **AT** port and which (if any) is the **NMEA**
GPS port. If there is no separate NMEA port, GPS runs over AT (`+QGPSLOC`,
PR #291) — leave `cell.gps.tty` empty.

---

## 8. Step 5 — bring up `cellular-client`

```bash
# Tell the daemon the ports + APN (read from ds at start):
ds-cli set cell.modem.tty '"/dev/ttyUSB2"'     # the AT port from Step 4
ds-cli set cell.gps.tty   '"/dev/ttyUSB1"'     # NMEA port, OR leave "" for AT GPS
ds-cli set cell.apn       '"<your-operator-apn>"'   # e.g. "airtelgprs.com"
# ⭐ FIRST confirm the modem selected the SIM you think it did — the board may be
#    on its soldered eSIM. `AT!UIMS?` must be 0 (external), not 1 (embedded).
#    See hw-bringup-wp7702-cellular-wan.md §2.

# One-shot foreground run to watch it talk to the modem:
sudo cellular-client --modem-tty=/dev/ttyUSB2
#   journal: "[cell] up: AT=/dev/ttyUSB2 GNSS=… apn=…"

# Verify telemetry:
ds-cli get cell.state        # searching → registered → connected
ds-cli get cell.operator     # e.g. "Vodafone"
ds-cli get cell.signal.dbm   # e.g. "-71"
ds-cli get gps.fix           # none → 2d → 3d  (needs sky view; can take minutes)
ds-cli get gps.lat ; ds-cli get gps.lon

# Enable the service (NOT auto-enabled — needs the WP module):
sudo systemctl enable --now iot-cellular-client.service
```

**UI:** **WAN → Cellular** (operator/signal/IP/ICCID) and **Sensors → Location
(GPS)** (fix + map link). **Cloud:** a server `Read`/`Observe` on **`/6/0/0..6`**
returns the live position via the lwm2m client's Object-6 mirror.

---

## 9. Decision points & contingencies

- **Transport (i2c-dev vs mmio):** prefer `/dev/i2c-1`. Use `--mmio` only if the
  node can't be created; that path needs `CAP_SYS_RAWIO` and may be blocked by
  `CONFIG_STRICT_DEVMEM`.
- **GPS source:** NMEA tty (`cell.gps.tty` set) **or** AT `+QGPSLOC`
  (`cell.gps.tty` empty). Both are wired; pick by what Step 4 found.
- **Data path (cellular):** `cellular-client` only owns status + GPS; the IP data
  path is the modem stack's job (open question §6.D). On the WP7702 the host-side
  data call on `wwan0` is **refused by firmware** (`AT$QCRMCALL` → `NO CARRIER`);
  the ECM link (`eth1` ↔ module `ecm0`) is what works. See
  [`hw-bringup-wp7702-cellular-wan.md`](hw-bringup-wp7702-cellular-wan.md).
- **⭐ Wrong-SIM trap:** the board may boot on its **soldered eSIM** rather than
  the card in the slot — every symptom looks healthy while zero bytes flow.
  Check `AT!UIMS?` (`1` = embedded) and `cm sim info` (`Type: EMBEDDED`) **before**
  debugging APNs. Fix: `AT!ENTERCND="A710"` then `AT!UIMS=0` + `AT+CFUN=0/1`.
- **Contingency A — sensors not on the Pi bus:** if `i2cdetect` is empty, the
  onboard sensors are on the WP's private CF3 bus, not the IoT connector. Then
  either (a) wire to the sensor bus test points if the board exposes them, or
  (b) read them with Legato on the WP and bridge over USB — a different
  architecture than `iot-sensord` (which assumes direct I²C). Capture the
  finding before re-planning.
- **Contingency B — sensors behind a mux/expander:** if a 0x70–0x77 device
  appears and the sensors don't, select the mux channel first (a small
  `I2cMux` shim) — record the mux address + channel map.

---

## 10. Sign-off checklist

```
I²C sensors
  [ ] /dev/i2c-1 present, group i2c
  [ ] i2cdetect shows 0x68, 0x76, 0x44 (note any expander 0x70–0x77)
  [ ] chip-ids: BMI160 0xD1, BME680 0x61, OPT3001 0x3001
  [ ] iot-sensord --once publishes iot.sensor.* ; UI Sensors tile live
  [ ] service enabled; privilege dropped to /dev/i2c-1 only

Cellular + GPS
  [ ] lsusb shows the WP; /dev/ttyUSB* enumerated
  [ ] AT port identified (AT → OK); NMEA port noted (or AT-GPS)
  [ ] SIM + APN set; cell.state reaches "registered"/"connected"
  [ ] gps.fix reaches 2d/3d; gps.lat/lon sane; UI Cellular + Location tiles live
  [ ] cloud Read /6/0/0..6 returns live location

Record actuals (addresses, ttyUSB mapping, operator, anomalies) back into
this file's §4/§7 so the next board is faster.
```

---

## 11. Troubleshooting quick-reference

| Symptom | Likely cause | Action |
|---------|-------------|--------|
| `/dev/i2c-1` missing | I²C not enabled | `raspi-config do_i2c 0` / `dtparam=i2c_arm=on` + reboot |
| `i2cdetect` all `--` | SDA/SCL swapped, no GND, wrong bus, or Contingency A | re-check wiring; `i2cdetect -l`; §9 A |
| chip present but id wrong | different part / strap | confirm part; set `--bme/--imu/--light=0xNN` |
| `iot-sensord` "no sensors responded" | bus up but reads NACK | check pull-ups, baudrate (drop to 100 kHz), addresses |
| `iot-sensord` falls back to mmio | `/dev/i2c-1` absent/denied | fix the node + group; don't ship on mmio |
| no `/dev/ttyUSB*` | WP not powered / cable / driver | check WP power LED; `dmesg | tail`; `usb_modeswitch` |
| AT port gives no `OK` | wrong ttyUSB or baud | try each port; baud 115200; check it's the AT (not DIAG) port |
| `cell.state` stuck "searching" | no SIM / no coverage / wrong APN | check SIM seated, antenna, `cell.apn` |
| `gps.fix` stays "none" | no sky view / GNSS off | move outdoors; ensure `cell.gps.enable=true`; wait (cold fix ~minutes) |
