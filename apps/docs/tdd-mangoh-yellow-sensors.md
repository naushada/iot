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
| **C — IPSO objects (client)** | 3 | ✅ **DONE** | `install_sensors(store, SensorHooks)` in `apps/{inc,src}/lwm2m_object_sensors.{hpp,cpp}`, called from `main.cpp` after `install_canonical_objects`. OIDs 3301/3303/3304/3315 (scalar 5700+5701) and 3313/3334 (tri-axis 5702-4+5701), all observable. Read closures pull `iot.sensor.*` from the data-store (unset → "0"); accel/gyro split the `"x,y,z"` csv. gtests in `apps/test/sensors_object_test.cpp`. (3325 Gas/VOC deferred with the BME680 gas driver.) |
| **D — ds keys (schema)** | 3 | ✅ **DONE** | `iot.sensor.{temp,humidity,pressure,lux,accel,gyro,version}` added to `modules/data-store/schemas/iot.lua` (Viewer; scalars string, version integer). `iot.sensor.version` bumps each sample for the device-ui long-poll. Reuses one `iot.sensor.*` namespace (no key-per-RID). device-ui tile = follow-up. |
| **E — iot-sensord daemon** | 4 | ✅ **DONE** (code) | `modules/sensors/`: `inc/sensor_reader.hpp` + `src/sample/` (`SensorCache` + `sample_all`, host-tested over `FakeI2cTransport`) and `daemon/` (`iot-sensord`: connects ds, maps BSC1+GPIO via `mmio.hpp`, `bus_init`, samples on an interval with `ACE_OS::sleep`/`ACE_DEBUG`, publishes `iot.sensor.*` via `Client::set`). `SENSORS_BUILD_DAEMON` (forced ON in the iot build) builds it linking `sensors_driver`+`datastore_client`+ACE. **Verified in podman (ubuntu+ACE+gtest): daemon compiles/links/runs; 12 i2c_bus + 22 sensors + 6 IPSO gtests pass.** |
| **Yocto packaging** | 4 | ✅ **DONE** | `iot_git.bb`: new `${PN}-sensord` package (binary + unit), unit shipped from `files/iot-sensord.service` (ExecStart `/usr/bin`), env via `${PN}-config`; **registered but `SYSTEMD_AUTO_ENABLE=disable`** (needs the mangOH board + `CAP_SYS_RAWIO`, so it would Restart-loop on a board without it — operator enables it). Added to `packagegroup-iot-full`. systemd unit grants `CAP_SYS_RAWIO` + `DeviceAllow=/dev/mem,/dev/gpiomem`. |
| F — HW bring-up | 4 | ⬜ TODO (needs HW) | `i2cdetect -y 1` to confirm addresses/parts/expander; correct defaults; confirm the `CAP_SYS_RAWIO`/`/dev/mem` model actually maps BSC1 under `CONFIG_STRICT_DEVMEM` (else fall back to a `/dev/i2c-1` ioctl transport); `systemctl enable --now iot-sensord`; device-ui tile; on-device validation. |

> **Privilege boundary (decided in PR-3):** the lwm2m client is unprivileged
> (`User=engineer`) and must not get `CAP_SYS_RAWIO`/`/dev/mem`. So sensor I/O
> lives in a separate **iot-sensord** daemon (PR-4) that publishes `iot.sensor.*`
> to the data-store; the client only *reads* those keys to fill the IPSO objects
> — exactly the ds-handoff already used for `iot.version`, `net.iface.active.ip`
> and `iot.update.*`. This replaces the earlier "in-process cache + sampler in
> the client" sketch (Layer 4).

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
hardware (task F).

PR-4 (host, via podman — builds the ACE daemon + gtest suites for real on Linux):

```bash
podman run --rm -i -v "$PWD":/src:Z --entrypoint bash ubuntu:22.04 -s <<'SH'
apt-get update -qq && apt-get install -y -qq build-essential cmake libgtest-dev \
    libace-dev liblua5.3-dev zlib1g-dev libssl-dev
# sensor driver + reader gtests
cmake -S /src/modules/sensors -B /tmp/s -DSENSORS_BUILD_TESTS=ON && cmake --build /tmp/s -j4
/tmp/s/test/sensors_test
# iot-sensord daemon (real ACE + datastore_client)
cmake -S /src/modules/sensors -B /tmp/d -DSENSORS_BUILD_DAEMON=ON -DACE_ROOT=/usr && \
    cmake --build /tmp/d -j4 --target iot-sensord
SH
```

> CI note: image workflows build on `main` only — review each PR's C++ before
> merge, since breaks surface post-merge. PR-4 was validated end-to-end in
> podman (ubuntu+ACE+gtest): daemon links & runs, all gtests green.

## 6. WAN plane — mangOH cellular uplink + GPS over LwM2M

The mangOH Yellow's CF3 cellular module (Sierra Wireless WP-series) is the Pi's
**primary WAN** and the **GPS/GNSS** source. Separate workstream from the I²C
sensor drivers (PRs 1–4) — it touches `modules/wan` + the LwM2M object set — but
shares the same destination: **cloud-iot over LwM2M**, on the infrastructure that
already exists (DTLS/PSK bootstrap+register, ClientRegistry →
`cloud.lwm2m.registrations` → EndpointRegistry → `cloud.endpoints`, observe/notify).
See `[[reference_cloud_endpoints_dataflow]]`. net-router already routes the
cellular slot (`net.iface.cellular.name` default `wwan0`,
`net.iface.priority="eth,wifi,cellular"`), so once the daemon brings up `wwan0`
with a default route, bootstrap→register→observe flow unchanged.

### 6.A Task breakdown

| Task | PR | State | Notes |
| --- | --- | --- | --- |
| **A — cellular core** | A | ✅ **DONE** | `modules/wan/cellular/` (`cellular_core` lib): pure `at_parser` (`+CSQ`→dBm/bars, `+COPS`→operator+tech, `+CREG/CGREG/CEREG`→reg, `+CGPADDR`→IP, ICCID), `nmea_parser` (GGA/RMC + XOR checksum → signed decimal-degree fix, knots→km/h), `CellularState` cache → `cell.*`/`gps.*` `to_kv()` batch. `schemas/cell.lua`. **17 gtests pass in podman.** No I/O — host-testable. |
| **B — GPS → Object 6** | B | ✅ **DONE** | `install_location` (OID 6) now takes `LocationHooks` (RID 0 Lat, 1 Lon, 2 Alt, 5 Timestamp, 6 Speed; all observable, unset → "0"), threaded through `install_canonical_objects`. `main.cpp` binds Lat/Lon/Alt/Speed to the `gps.*` ds keys (Timestamp static until the daemon emits an epoch). gtests in `apps/test/location_object_test.cpp`. **Built+passing in podman (real installers + ACE/lua linked).** |
| **C — cellular-client daemon** | C | ✅ **DONE** (code) | `modules/wan/cellular/daemon/`: `SerialChannel` (`ACE_TTY_IO`, registered with the `ACE_Reactor`; `handle_input` → `LineAssembler` → `dispatch_*`), `CellularClient` (ds connect, config from `cell.*` keys, poll timer issues `AT+CSQ/+COPS?/+CEREG?/+CGPADDR=1/+QCCID`, lifecycle token from reg+IP, publishes `cell.*`/`gps.*`). Pure `line_router` (assembly + AT/NMEA dispatch) is host-tested. **Built+linked against real ACE + datastore_client in podman; runs.** Yocto `${PN}-cellular` package + `cellular-client.service` (`SYSTEMD_AUTO_ENABLE=disable` — needs the WP module). |

### 6.B Daemon I/O design — ACE only, reactor-driven (decided)

Per `[[feedback_ace_over_posix]]`, the daemon (PR-C) uses **ACE for all I/O — no
raw POSIX** `open/read/write/select`:
- The AT control channel and the GNSS NMEA stream are **serial ttys**
  (`/dev/ttyUSB*`), opened via **`ACE_TTY_IO`/`ACE_DEV_IO`** (termios setup
  through ACE), not `::open`.
- Each device's `ACE_HANDLE` is **registered with the `ACE_Reactor`** through an
  `ACE_Event_Handler` (`get_handle()` returns the fd; `handle_input()` fires on
  readable) — i.e. yes, a driver's fd is fed to the reactor; no polling loop.
  `handle_input()` line-buffers and feeds complete lines to the **pure PR-A
  parsers**. A reactor **timer** (`schedule_timer`) drives the periodic AT polls
  (`+CSQ/+COPS/+CREG`) and the GNSS query. Any sockets use `ACE_SOCK_*`; sleeps
  `ACE_OS::sleep`; logging `ACE_DEBUG/ACE_ERROR`.
- This keeps the daemon a thin reactor shell around the already-tested parsing/
  state core.

### 6.C GPS / GNSS source
GPS comes off the **same WP module** (a GNSS NMEA tty, or `+QGPSLOC` over AT),
**not** I²C. The daemon parses the fix (PR-A `nmea_parser`/`at_parser`) →
publishes `gps.*` → PR-B mirrors into **LwM2M Object 6 (Location)** RIDs 0/1/2/5/6
with observe/notify on position change, so the cloud Endpoints table can show
device location.

### 6.D Open questions (WAN — need hardware)
1. Modem stack: ModemManager/`mmcli` vs. raw `qmicli`/`pppd` (or pure-AT
   `+CGACT`/`+QNETDEVCTL`) — pick per image size / WP firmware (QMI vs MBIM).
2. USB enumeration: which interfaces the WP exposes (AT vs DIAG vs NMEA vs net),
   composition may need `usb_modeswitch`.
3. SIM/APN provisioning path (operator profile via device-ui vs. preconfigured
   `cell.apn`).
4. Power: WP + Pi draw on the mangOH supply during TX peaks.
