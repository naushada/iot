# sensors — mangOH Yellow I²C sensor drivers

Per-chip drivers for the mangOH Yellow sensor block, read from a Raspberry Pi 3B
over I²C. Each driver depends only on the **`I2cTransport`** seam from
[`modules/bcm2837`](../bcm2837) (`i2c_bus.hpp`), so it runs over real BSC1 MMIO
on the Pi and over an in-memory fake on the host — the suite needs no hardware.

| Chip | Class | Bus addr | Provides | IPSO (PR-3) |
|------|-------|----------|----------|-------------|
| Bosch **BMI160** | `Bmi160` | 0x68 / 0x69 | accel + gyro (raw int16 ×3) | 3313 / 3334 |
| Bosch **BME680** | `Bme680` | 0x76 / 0x77 | temperature, pressure, humidity¹ | 3303 / 3315 / 3304 |
| TI **OPT3001**²  | `Opt3001` | 0x44 | illuminance (lux) | 3301 |

¹ Gas/VOC resistance (IPSO 3325) needs heater-profile setup and is a follow-up.
² The exact mangOH light-sensor part is confirmed on hardware (TDD open Q#1);
  OPT3001 is the default fit.

## Pattern

```
inc/i2c_sensor.hpp     I2cSensor base: holds I2cTransport& + 7-bit addr,
                       read_regs / read_u8 / write_u8 helpers
inc/<chip>.hpp         driver class: probe() / init() / read(...)
src/<chip>/<chip>.cpp  register map + decode (anon-namespace constants)
test/                  gtests over FakeI2cTransport (a 256-byte auto-increment
                       register file); compensation primitives are pure + tested
```

## Build & test

```bash
cmake -S modules/sensors -B modules/sensors/build -DSENSORS_BUILD_TESTS=ON
cmake --build modules/sensors/build
ctest --test-dir modules/sensors/build
```

The library target is `sensors_driver` (alias `sensors::driver`); it links
`bcm2837_driver` transitively and is consumed from the iot apps build in PR-3.

> The BME680 fixed-point compensation follows the Bosch BME68x datasheet/
> reference driver. Host tests cover register protocol, calibration-packing
> decode, axis/lux decode, and compensation behaviour (monotonicity, clamping,
> determinism, div-by-zero guard); exact physical accuracy is cross-checked on
> hardware in PR-4.
