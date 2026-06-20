# TDD Plan — mangOH Yellow Sensor Integration over RPi I²C

Status: **IN PROGRESS** — PR-1 (BCM2837 I²C transaction layer) implemented and
host-unit-tested; PRs 2–4 designed below, not yet started.

Wire the [mangOH Yellow](https://www.bipom.com/documents/mangOH_Yellow_User_Guide.pdf)
sensor block to a Raspberry Pi 3B (BCM2837) over **I²C** and surface its
readings through the existing LwM2M / data-store / device-ui stack.

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
| **A — I²C transaction layer** | 1 | ✅ **DONE** | `modules/bcm2837` `inc/i2c_bus.hpp` + `src/i2c/i2c_bus.cpp`: `I2cTransport` seam, `Bcm2837I2cTransport` (`bus_init` = GPIO2/3→ALT0 + divider + enable; `write`/`read`/`write_read`/`read_reg`/`write_reg`; `I2cResult` Ok/BadArg/Timeout/Nack/ClockTimeout). Bounded-spin FIFO pump on S.DONE with S.ERR/S.CLKT checks. Host gtests in `test/i2c_bus_test.{hpp,cpp}`. |
| B — sensor drivers | 2 | ⬜ TODO | `modules/sensors/`: `Bmi160` (chip-id 0xD1, addr 0x68/0x69), `Bme680` (addr 0x76/0x77, Bosch calib compensation), `LightSensor` (confirm part; typ 0x44). Each ctor takes `I2cTransport&`; host tests inject a `FakeI2cTransport` with canned register maps. Add `I2cMux`/expander shim **iff** `i2cdetect` shows the sensors gated behind the mangOH expander. |
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

The new `I2cBus*` tests run over `std::vector`-backed I²C/GPIO blocks (same seam
as the existing suite) — no Pi required. Real DONE/FIFO/RX behaviour is only
exercised on hardware in PR-4.

> CI note: image workflows build on `main` only and there is no local C++
> toolchain on the dev Mac — review each PR's C++ carefully before merge, since
> breaks surface post-merge.
