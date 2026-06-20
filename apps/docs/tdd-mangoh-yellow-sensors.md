# TDD Plan — mangOH Yellow Integration over RPi (I²C sensors + cellular WAN)

Status: **IN PROGRESS** — PR-1 (BCM2837 I²C transaction layer) and PR-2
(per-chip sensor drivers) implemented and host-unit-tested; PRs 3–4 (LwM2M
wiring + HW bring-up) designed below. The cellular-WAN / GPS plane (§6) is
scoped but not yet broken into PRs.

Wire the [mangOH Yellow](https://www.bipom.com/documents/mangOH_Yellow_User_Guide.pdf)
to a Raspberry Pi 3B (BCM2837). Two planes:

1. **Sensor plane (this doc's PRs 1–4):** read the onboard BMI160 / BME680 /
   light sensors over **I²C** and surface them through the existing
   LwM2M / data-store / device-ui stack.
2. **WAN plane (§6):** use the mangOH Yellow's CF3 **cellular module** as the
   Pi's primary WAN uplink, and pull **GPS/GNSS** off the same module. All data
   — sensors, GPS, device telemetry — flows to **cloud-iot over LwM2M**, reusing
   the infrastructure already in place (ClientRegistry → cloud.endpoints, IPSO
   objects, DTLS/PSK, observe/notify).

## 0. Hardware decision: I²C, not SPI

Every onboard sensor on the mangOH Yellow is **I²C** — BMI160 (accel/gyro),
BME680 (pressure/temperature/humidity/gas/VOC), and the ambient light sensor
all sit on the CF3 module's I²C bus (some behind a GPIO/I²C expander). None is
SPI. We therefore use the Pi's **BSC1** controller (`modules/bcm2837` `I2C`
driver, phys `0x3F804000`, pins **GPIO2/SDA1 + GPIO3/SCL1** at ALT0). SPI0 is
left untouched.

> Physical bring-up: wire the mangOH IoT/expansion connector's I²C
> SDA/SCL/3V3/GND to Pi pins 3 (GPIO2) / 5 (GPIO3) / 1 (3V3) / 6 (GND). Confirm
> 3.3 V levels and that pull-ups are present on exactly one side before
> connecting.

## 1. Architecture (4 layers, bottom-up)

```
 Layer 4  ACE-timer sampler (in iot binary)  ── reads sensors at interval N,
          + ds publish (iot.sensor.*)            caches values, updates ds keys
 Layer 3  LwM2M IPSO objects  ── install_sensors() next to install_device();
          (3303/3304/3315/3325/3313/3334/3301)   read-closures call Layer 2
 Layer 2  Per-chip drivers (modules/sensors/) ── Bmi160 / Bme680 / LightSensor,
          depend only on the I2cTransport seam     host-testable via a fake
 Layer 1  BCM2837 I²C transaction layer  ── I2cTransport + Bcm2837I2cTransport
          (modules/bcm2837)                       bus_init (ALT0 + divider) +
                                                  transmit/receive/write_read
```

The key architectural seam is the **`I2cTransport`** abstract interface
(Layer 1): every sensor driver (Layer 2) depends on it, never on the BSC
registers directly, so sensors are unit-tested on the host against a
`FakeI2cTransport` — the same dependency-injection idiom the LwM2M object
readers already use.

## 2. Why a transaction layer was needed first

`docs/DRIVER_REVIEW.md §3` ships the BSC1 `I2C` driver as a **host-side
bit-layout model**: it has `slave_address/data_length/start_read/start_write/
read_byte` but no validated end-to-end transfer on silicon — no GPIO ALT0
pin-mux, no clock-divider/enable setup, no FIFO pump against the DONE/ERR/CLKT
status bits, and no register-read primitive (write reg pointer, then read N
bytes) which is exactly how BMI160/BME680 are read. PR-1 closes that for the
I²C path.

## 3. Task breakdown

| Task | PR | State | Notes |
| --- | --- | --- | --- |
| **A — I²C transaction layer** | 1 | ✅ **DONE** (#283) | `modules/bcm2837` `inc/i2c_bus.hpp` + `src/i2c/i2c_bus.cpp`: `I2cTransport` seam, `Bcm2837I2cTransport` (`bus_init` = GPIO2/3→ALT0 + divider + enable; `write`/`read`/`write_read`/`read_reg`/`write_reg`; `I2cResult` Ok/BadArg/Timeout/Nack/ClockTimeout). Bounded-spin FIFO pump on S.DONE with S.ERR/S.CLKT checks. Host gtests in `test/i2c_bus_test.{hpp,cpp}`. |
| **B — sensor drivers** | 2 | ✅ **DONE** | `modules/sensors/` (`sensors_driver` lib): `Bmi160` (chip-id 0xD1, raw int16 axes), `Bme680` (chip-id 0x61, calibration-packing decode + Bosch fixed-point T/P/H compensation; gas TODO), `Opt3001` (device-id 0x3001, lux = 0.01·2^E·mantissa). `I2cSensor` base over the `I2cTransport` seam; gtests over a `FakeI2cTransport` (256-byte auto-increment register file). Add an `I2cMux`/expander shim **iff** `i2cdetect` shows the sensors gated behind the mangOH expander (PR-4 finding). |
| C — IPSO objects | 3 | ⬜ TODO | `install_sensors(store, hooks)` in the object layer, called from `install_canonical_objects` (`apps/src/lwm2m_object_stubs.cpp`). OIDs: 3303 Temp, 3304 Humidity, 3315 Barometer, 3325 Gas/VOC, 3313 Accel, 3334 Gyro, 3301 Illuminance. Resource `read` closures return the Layer-4 cache. Observe/Notify rides the existing observe machinery (NON default, every Nth CON). |
| D — ds keys + device-ui | 3 | ⬜ TODO | Publish `iot.sensor.*` so the local device-ui shows live values without a cloud round-trip. Reuse/extend an existing sensor namespace — do not mint a key per resource. Add a device-ui tile. |
| E — sampler | 3 | ⬜ TODO | Periodic reader on the **ACE reactor timer** (StatsPublisher pattern), ACE_DEBUG/ACE_ERROR logging. Updates the IPSO read-closure cache + ds keys at interval N. Behind a build/PACKAGECONFIG flag + runtime config so a board without the mangOH attached no-ops. |
| F — HW bring-up | 4 | ⬜ TODO | `i2cdetect -y 1` to confirm addresses/parts; correct any defaults; on-device validation; Yocto recipe `mangoh-sensors` PACKAGECONFIG. |

## 4. Open questions (need hardware)

1. **Exact light-sensor part + all I²C addresses** — confirm with `i2cdetect`
   and the HW guide schematic. Addresses in Task B are typical defaults only.
2. **I²C expander/mux** — is the sensor block gated behind a mangOH expander?
   If so, Task B needs the expander shim first.
3. **Levels / pull-ups** — confirm the IoT-connector I²C is 3.3 V (Pi-safe) and
   that pull-ups exist on exactly one side (avoid double pull-ups).
4. **Connector pinout** — which mangOH connector exposes SDA/SCL/3V3/GND.

## 5. Build & test

PR-1 (host, no hardware):

```bash
cmake -S modules/bcm2837 -B modules/bcm2837/build -DBCM2837_BUILD_TESTS=ON
cmake --build modules/bcm2837/build
ctest --test-dir modules/bcm2837/build      # or ./modules/bcm2837/build/test/bcm2837_test
```

PR-2 (host, no hardware):

```bash
cmake -S modules/sensors -B modules/sensors/build -DSENSORS_BUILD_TESTS=ON
cmake --build modules/sensors/build
ctest --test-dir modules/sensors/build      # gtests over FakeI2cTransport
```

Both suites run over `std::vector`/in-memory register blocks — no Pi required.
Real DONE/FIFO/RX behaviour and sensor numeric accuracy are exercised on
hardware in PR-4.

> CI note: image workflows build on `main` only — review each PR's C++ before
> merge, since breaks surface post-merge.

## 6. WAN plane — mangOH cellular uplink + GPS over LwM2M

The mangOH Yellow's CF3 cellular module (Sierra Wireless WP-series) is the Pi's
**primary WAN** and the **GPS/GNSS** source. This is a separate workstream from
the I²C sensor drivers (PRs 1–4) — it touches `modules/wan` and the LwM2M object
set, not `modules/sensors` — but shares the same destination: **cloud-iot over
LwM2M**, on the infrastructure that already exists (DTLS/PSK bootstrap+register,
ClientRegistry → `cloud.lwm2m.registrations` → EndpointRegistry → `cloud.endpoints`,
observe/notify, IPSO objects). See `[[reference_cloud_endpoints_dataflow]]`.

### 6.1 Cellular WAN
- The WP module presents to the Pi over USB as a network interface (QMI/MBIM or
  RNDIS/ECM) plus an AT control channel. Bring-up = enumerate modem → set APN →
  connect (ModemManager/`mmcli` or a `qmicli`/`pppd` path) → default route.
- Add a **`modules/wan/cellular/`** daemon as a sibling of `modules/wan/wifi/`
  (mirrors the wifi-client structure): manage the connection, publish
  `wan.cellular.*` state into the data-store (operator, RSSI/RSRP, APN, IP,
  up/down), and integrate with the existing online-gate / route preference so
  cellular is the uplink when wired/WiFi is absent. Reuse the `vpn.state` /
  `iot.conn.state` lifecycle pattern, not a new bespoke key
  (`[[feedback_ds_key_reuse]]`).
- The VPN/LwM2M client already binds to "whatever the default route is", so once
  cellular provides the route, bootstrap→register→observe flows unchanged.

### 6.2 GPS / GNSS → LwM2M Object 6 (Location)
- GPS comes off the **same WP module** (AT `+GPS`/`+QGPS` or QMI LOC / a gnss
  NMEA device), **not** I²C — so it lives in the cellular daemon, which parses
  fix → publishes `gps.*` (lat/lon/alt/speed/ts) into ds.
- Surface to the cloud as **LwM2M Object 6 (Location)**: RIDs 0 Latitude,
  1 Longitude, 2 Altitude, 5 Timestamp, 6 Speed. Install it next to the IPSO
  sensor objects (PR-3 `install_sensors` sibling, e.g. `install_location`), with
  read-closures bound to the `gps.*` ds keys and observe/notify on position
  change. The cloud Endpoints table can then show device location.

### 6.3 Telemetry transport recap
Everything converges on LwM2M: sensors → IPSO 33xx, GPS → Object 6, device
health → Object 3 + existing `iot.*` keys. No new cloud ingestion path is
needed — the device just registers more objects, and the cloud observes them.

### 6.4 Open questions (WAN)
1. Modem stack on the Yocto image: ModemManager vs. raw `qmicli`/`pppd` — pick
   per image size / WP firmware (QMI vs MBIM).
2. USB enumeration: which interface the WP exposes to the Pi (composition may
   need a `usb_modeswitch` / mode set).
3. SIM/APN provisioning path (operator profile via device-ui vs. preconfigured).
4. Power: the WP + Pi current draw on the mangOH supply during TX peaks.
