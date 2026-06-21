# modules/vehicle — Vehicle telemetry over CAN (ISO 15765-4 / OBD-II)

Greenfield CAN/OBD-II telemetry. See the design at
`apps/docs/tdd-vehicle-telemetry.md`.

## Layout (mirrors `modules/wan/cellular`)

| Path | Role |
| --- | --- |
| `inc/`, `src/` | **Pure core** — OBD-II PID encode/decode + DTC formatting. No ACE / SocketCAN / data-store, so it is fully host-unit-testable (`vehicle_core`). |
| `test/` | gtest suite (`vehicle-tests`) over the pure core. |
| `daemon/` | *(later PR)* `iot-vehicled` — reactor-driven SocketCAN poller, publishes `vehicle.*` to the data-store. |
| `schemas/` | *(later PR)* `vehicle.lua` ds keys. |

## Build + test (host)

```sh
cmake -S modules/vehicle -B build/vehicle
cmake --build build/vehicle
ctest --test-dir build/vehicle --output-on-failure
```

`VEHICLE_BUILD_TESTS` defaults ON for a top-level build, OFF when the module is
pulled into the iot image via `add_subdirectory`. `VEHICLE_BUILD_DAEMON` (OFF by
default) gates the ACE/SocketCAN daemon, forced ON in the image build.

## OBD-II decode (`vehicle::obd`, SAE J1979 Mode 01)

Request on functional id `0x7DF`: `[0x02, mode, pid, pad…]`; response on
`0x7E8..0x7EF`: `[len, mode|0x40, pid, A, B, …]`.

| PID | Signal | Decode | Unit |
| --- | --- | --- | --- |
| `0x04` | Engine load | `A·100/255` | % |
| `0x05` | Coolant temp | `A−40` | °C |
| `0x0C` | RPM | `(256A+B)/4` | rpm |
| `0x0D` | Speed | `A` | km/h |
| `0x0F` | Intake air temp | `A−40` | °C |
| `0x10` | MAF | `(256A+B)/100` | g/s |
| `0x11` | Throttle | `A·100/255` | % |
| `0x2F` | Fuel level | `A·100/255` | % |

DTCs (Mode 03 / SAE J2012): `decode_dtc(hi, lo)` → `Pxxxx`/`Cxxxx`/`Bxxxx`/`Uxxxx`.
