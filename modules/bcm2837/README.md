# rpi3B — bare-metal peripheral drivers for the Raspberry Pi 3B (BCM2837)

A small, header-light C++ driver layer for the Broadcom **BCM2837** SoC on the
Raspberry Pi 3B. Each peripheral is modelled as a memory-mapped register block
that the driver classes read/modify/write through typed, field-level accessors.

Peripherals: **GPIO**, **Clock manager (CM_GPn)**, **Interrupt controller**,
**I2C (BSC1)**, **SPI0**, **System Timer** (1 MHz free-running counter +
compare channels; backs the interrupt-I²C watchdog deadline).

> See [`docs/DRIVER_REVIEW.md`](docs/DRIVER_REVIEW.md) for the design review,
> the per-peripheral issue log, and what is fixed vs. still open.

---

## Framework

Every peripheral follows the same three layers:

| Layer | File | Responsibility |
|-------|------|----------------|
| Register block | `inc/memory_map.hpp` | `enum Register` (word offsets), field enums, and a placement `operator new` returning the peripheral's **physical base** on hardware (or a caller-supplied buffer in tests). |
| Driver class | `inc/<peri>.hpp` | Holds a reference to the register block; exposes field `set/clr/get` + helpers. |
| Implementation | `src/<peri>/<peri>.cpp` | Bit manipulation against `m_register[...]`. |

The placement-`new` trick lets the **same code** run two ways:

```cpp
GPIO gpio;                         // hardware: registers overlay MMIO at 0x3F200000
                                   // (real Pi only — see "Status" below)

std::vector<std::uint32_t> buf(BCM2837::GPIORegistersAddress::BCM2837_MAX);
GPIO gpio(buf.data());             // test/host: registers overlay a heap buffer
```

Physical bases: GPIO `0x3F200000`, Clock `0x3F101070`*, IRQ `0x3F00B200`,
I2C/BSC1 `0x3F804000`, SPI0 `0x3F204000`, System Timer `0x3F003000`.
<sub>*Clock base is tracked as an open issue — see DRIVER_REVIEW §2.3.</sub>

---

## Build

Requires CMake ≥ 3.16 and a C++20 compiler. GoogleTest is only needed for the
test suite.

```bash
cmake -S . -B build
cmake --build build
```

Targets / options:

| Target / option | Default | Purpose |
|-----------------|---------|---------|
| `bcm2837_driver` (alias `bcm2837::driver`) | always | STATIC driver library (`src/*/*.cpp`). |
| `bcm2837_demo` | top-level only | Standalone demo executable (`src/main.cpp`). |
| `bcm2837_test` | top-level only | GoogleTest suite. |
| `-DBCM2837_BUILD_APP=ON/OFF` | top-level | Build the demo executable. |
| `-DBCM2837_BUILD_TESTS=ON/OFF` | top-level | Build + register the gtest suite. |

When pulled in via `add_subdirectory(...)`, only `bcm2837_driver` is built by
default (app/tests off), so a parent project just links the library:

```cmake
add_subdirectory(bcm2837)
target_link_libraries(my_app PRIVATE bcm2837_driver)   # headers come transitively
```

The public headers are C++17-consumable (the `auto`-param ctors are written as
explicit templates), so a C++17 parent can link the C++20-built library.

### Tests

```bash
cmake -S . -B build -DBCM2837_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build            # or: ./build/test/bcm2837_test
```

Tests construct each driver over a `std::vector<uint32_t>` and assert the
register **bit-layout** — no hardware required.

---

## Usage sketches

```cpp
// GPIO — drive GPIO17 as an output, set it high
GPIO gpio;
gpio.output(17);
gpio.GPSETn(17);

// I2C (BSC1) — address a device and start a write
I2C i2c;
i2c.enable();
i2c.slave_address(0x48);
i2c.data_length(2);
i2c.write_byte(0xAB);
i2c.start_write();

// SPI0 — mode 0, CE0, one byte
SPI spi;
spi.mode(/*cpol=*/0, /*cpha=*/0);
spi.chip_select(0);
spi.clear_fifo();
spi.begin_transfer();
spi.write_byte(0x55);
```

> Register addressing is by **BCM GPIO number** (not 40-pin header pin). I2C/SPI
> additionally require the relevant pins muxed to ALT0 (GPIO2/3 for I2C1,
> GPIO7–11 for SPI0) and the peripheral clock divided appropriately.

---

## Real hardware (Linux MMIO)

On a booted Pi the drivers can attach to live registers through
[`inc/mmio.hpp`](inc/mmio.hpp), which `mmap`s a peripheral block and hands the
virtual pointer to the same region constructor the tests use:

```cpp
#include "mmio.hpp"

auto gpio = BCM2837::map_gpio();   // maps the GPIO block via /dev/mem (root)
gpio->output(17);
gpio->GPSETn(17);                // drive GPIO17 high
```

`map_gpio()` / `map_clock()` / `map_i2c()` / `map_spi()` / `map_systimer()` use `/dev/mem` (needs
root / `CAP_SYS_RAWIO`); `map_gpiomem()` maps **GPIO only** via the unprivileged
`/dev/gpiomem` (group `gpio`). Each returns an RAII `Mapped<Driver>` that owns
the mapping and unmaps on scope exit. The demo binary does this under
`bcm2837_demo --blink`.

This is safe because `BCM2837::mmio_reg` (the register cell type) is *trivially
default-constructible*: placement-new'ing a register block over live MMIO
**overlays** the registers instead of zeroing them. `IRQ` is **not** exposed
here — its IVT/exception-vector model is bare-metal-only, not a Linux userspace
concept.

## Status

The register/field logic is verified on the host (gtest over heap buffers) and
the same code drives real silicon via the `mmap` path above. What's still
bare-metal-only: there is no freestanding/cross toolchain, linker script, or
`_start`, and the `IRQ`/IVT layer assumes a kernel context. Driving real pins
also needs the usual pin-mux (ALT0) + clock-divider setup, not just the register
writes.

## Use in the iot Yocto image

This repo is vendored into [`naushada/iot`](https://github.com/naushada/iot)
under `modules/bcm2837` and built as the `bcm2837_driver` library. Its
gtest suite ships as the **`iot-bcm2837-selftest`** systemd oneshot, which runs the
bit-layout suite once at boot and records pass/fail in the journal (gated by the
recipe's `bcm2837-selftest` PACKAGECONFIG).
