# RPi3B (BCM2837) Bare-Metal Driver — Review & Issue Log

This document captures (1) how the driver framework is structured, and
(2) the defects found while reviewing the originally half-finished GPIO / Clock /
Interrupt drivers. It is the reference for the I2C/SPI work, which follows the
same framework while avoiding the bugs listed here.

**Status (current):** The **GPIO** and **Interrupt** defects (§2.2, §2.4) are
**fixed and merged** (PR #1), with regression tests. **I2C (BSC1)** and **SPI0**
are **implemented and shipped** (§3). The build is now library-consumable
(`bcm2837_driver` static lib) and the repo is vendored into the `iot` Yocto image
as the `iot-bcm2837-selftest` boot oneshot. The **Clock** defects (§2.3) and the
remaining design notes in §2.1 are **still open**.

---

## 1. Framework architecture

The project is a bare-metal C++17 driver targeting the BCM2837 SoC on the
Raspberry Pi 3B. Every peripheral is modelled with the **same three-layer
pattern**:

```
memory_map.hpp                inc/<peri>.hpp              src/<peri>/<peri>.cpp
+------------------------+    +--------------------+      +--------------------+
| <Peri>RegistersAddress |    | class <PERI>       |      | field get/set/clr  |
|  - enum Register {...}  | <- |  RegBlock& m_memory| <--  | bit manipulation   |
|  - enum field configs   |    |  accessor methods  |      |                    |
|  - operator new(region) |    +--------------------+      +--------------------+
|  - device_register[]    |
+------------------------+
```

### 1.1 Register block struct (`memory_map.hpp`)
Each peripheral has a POD-like struct, e.g. `GPIORegistersAddress`, containing:

- `enum Register : uint32_t { ... MAX }` — the **word offsets** of each hardware
  register (index 0,1,2… ⇒ byte offset 0x0,0x4,0x8…).
- Config enums (e.g. `Config`, `PullUpDownConfig`) for field values.
- `using device_register = volatile std::atomic<std::uint32_t>;`
- `device_register m_register[Register::MAX];` — the actual register window.
- An overloaded **placement `operator new(size_t, void* region)`**:
  - `region == nullptr` → returns the real peripheral **physical base address**
    (e.g. GPIO `0x3F200000`), so on hardware the struct overlays MMIO.
  - `region != nullptr` → returns `region`, so unit tests can place the struct
    over an ordinary heap buffer (`std::vector<uint32_t>`).

### 1.2 Driver class (`inc/<peri>.hpp`)
Holds a **reference** to the register block, bound in the constructor:

```cpp
GPIO()              : m_memory(*new BCM2837::GPIORegistersAddress) {}     // hardware
GPIO(auto region)   : m_memory(*new(region) BCM2837::GPIORegistersAddress) {}  // test
```

Methods read/modify/write fields of `m_memory.m_register[...]`.

### 1.3 Tests (`test/<peri>_test.{hpp,cpp}`)
A GoogleTest fixture allocates `std::vector<uint32_t>(Register::MAX)` and
constructs the driver over it (`m_driver(DRIVER(m_memory_region.data()))`), so
register reads/writes hit the vector instead of real MMIO. Tests therefore only
verify the **bit-layout arithmetic**, not real hardware behaviour.

### 1.4 Build
- Root `CMakeLists.txt` builds the `bcm2837_demo` executable as a **hosted** binary.
- `test/CMakeLists.txt` builds `bcm2837_test` and explicitly lists the peripheral
  `.cpp` files plus GoogleTest.

---

## 2. Issues found

Severity legend: **[H]** breaks correct behaviour on hardware · **[M]** logic
bug / inconsistency · **[L]** cosmetic / warning.

### 2.1 Build / framework wide
- **[H] Root executable never compiles the drivers.** ✅ **FIXED.** Root
  `CMakeLists.txt` used `file(GLOB SOURCES "src/*.cpp")`, but the driver sources
  live in `src/gpio/`, `src/clock/`, `src/interrupt/` — so only `src/main.cpp`
  was compiled and the project never actually built the drivers (it also failed
  under `-std=c++17` on the headers' abbreviated-template ctors). Resolved by the
  CMake refactor: a `bcm2837_driver` STATIC library globs `src/*/*.cpp`, builds as
  C++20, and the public headers are kept C++17-consumable (explicit-template
  ctors) so the C++17 `iot` build can link it.
- **[H] No freestanding/cross toolchain.** The project compiles as a normal host
  program (`-pg`, default sysroot). There is no `aarch64-none-elf` toolchain
  file, no linker script, no startup/`_start`, so the artifact is **not loadable
  on the Pi**. The `operator new` returning `0x3F200000` would segfault if ever
  dereferenced on the host. Today the framework is effectively a host-side
  bit-layout simulator.
- **[M] `volatile std::atomic<uint32_t>` for MMIO.** ✅ **FIXED.** `std::atomic`
  added no value over the peripheral bus, and—worse for the real-hardware
  path—C++20's `std::atomic` default constructor value-initialises to 0, so
  placement-new'ing a register block over live MMIO would **zero every
  register**. `device_register` is now `BCM2837::mmio_reg`, a trivially
  default-constructible `volatile uint32_t` wrapper whose compound operators do
  an explicit volatile load/modify/store (no `-Wdeprecated-volatile`). This lets
  `inc/mmio.hpp` overlay live registers via `/dev/mem` without clobbering them.
- **[L] Include guards on `.cpp` files** (`#ifndef __gpio_cpp__`) — harmless but
  unusual.
- **[L] Debug `printf` loop in `InterruptRegisterAddress` ctor** runs on every
  construction and emits `-Wformat`/`-Wsign-compare` warnings (prints register
  pointers).

### 2.2 GPIO (`src/gpio/gpio.cpp`) — ✅ FIXED
> Resolved by rewriting `gpio.cpp` to index every register directly by BCM GPIO
> number (the `gpioNo2PinNo` indirection was removed), adding the missing `else`
> branches, making `output()` clear-then-set, and adding a `gpio <= 53` range
> guard. Regression tests added in `test/gpio_test.cpp`.

- **[H] FSEL/SET/CLR indexing uses the *physical header pin* number, not the
  *BCM GPIO* number.** `gpioNo2PinNo` maps BCM GPIO → 40-pin-header pin, then the
  code indexes `GPFSELn` with `pin_no/10` and shifts by `(pin_no%10)*3`. Hardware
  `GPFSEL`/`GPSET`/`GPLEV` registers are organised **by GPIO number** (GPIO0–9 in
  GPFSEL0, …), so this programs the wrong pin on real hardware. The round-trip
  tests pass only because the same wrong index is used for both write and read.
  The physical pin number is irrelevant to register addressing and the
  indirection should be removed.
- **[H] Missing `else` on every "pin ≥ 32" branch.** In `GPSETn`, `GPCLRn`,
  `GPLEVn(setter)`, `GPGETn`, `GPEDSn`, `GPRENn`, `GPFENn`, `GPLENn`, `GPARENn`,
  `GPAFENn`, `GPPUDCLKn` the pattern is:
  ```cpp
  if (pin_no < 32) { reg0 |= (1U << pin_no); }
  reg1 |= (1U << (pin_no - 32));   // <-- runs even when pin_no < 32
  ```
  When `pin_no < 32`, `pin_no - 32` underflows (unsigned) and the shift is `>= 32`
  (undefined behaviour), and it wrongly touches the `…1` register. Only `GPHENn`
  has the correct `else`. All the others need the `else`.
- **[M] `output()` does not clear the FSEL field before setting.** It does
  `m_register[..] |= (1U << ((pin%10)*3))`, setting only bit 0 of the 3-bit field
  without clearing it first. Switching a pin that was an alt-function to output
  yields a corrupted mode (e.g. `0b100 → 0b101`). It should clear the 3 bits then
  write `0b001` (as `write()` does).
- **[M] `output()` vs `input()` index inconsistency.** `output()` indexes FSEL by
  `pin_no`; `input()` indexes by `gpio_n`. They disagree with each other and with
  `read()`/`write()` (which use `pin_no`). `input()` is currently untested so the
  bug is latent.
- **[M] GPSET/GPCLR modelled as plain latches.** Hardware `GPSET`/`GPCLR` are
  write-1-to-act, read-undefined registers; `GPGETn` reads back `GPSET` which has
  no hardware meaning. Acceptable for the simulator, but not hardware-faithful.
- **[L] Unused `pin_no` in `input()`; unused `gpio_n` params in `GPPUD`.**

### 2.3 Clock (`src/clock/clock.cpp`)
- **[H] Clock manager base address is wrong.** `operator new` returns
  `0x3F000000 + 0x00100000 = 0x3F100000`. The GP clock control registers
  (`CM_GP0CTL`…) are at physical **`0x3F101070`** (bus `0x7E101070`). The `0x1070`
  offset is missing, so on hardware this points into the wrong block.
- **[H] Missing clock password (0x5A) on writes.** Every write to a
  `CM_*CTL`/`CM_*DIV` register must carry `PASSWD = 0x5A` in bits [31:24], else the
  hardware **ignores the write**. No clock write sets it.
- **[H] Two conflicting bit layouts for CTL ENAB/KILL/MASH.** The generic
  `set_CM_GPnCTL` uses ENAB`<<4`, KILL`<<5`, MASH`<<8`; the per-register
  `CM_GP0CTL`/`CM_GP1CTL`/`CM_GP2CTL` use ENAB`<<5`, KILL`<<6`, MASH`<<10`. Per
  the datasheet the correct layout is **ENAB bit4, KILL bit5, MASH bits[10:9]**.
  So `set_CM_GPnCTL` is right for ENAB/KILL but wrong for MASH (should be `<<9`),
  and the per-register variants are wrong for all three.
- **[M] `get_CM_GPnDIV` read masks are wrong.** DIVF reads
  `reg & ~(1U<<12)` (clears only bit 12) and BOTH reads `reg & ~(1U<<24)`. They
  should mask the low 12/24 bits: `& ~((~0U)<<12)` / `& ~((~0U)<<24)` — exactly
  what the per-register `CM_GPnDIV` reads already do. Inconsistent.
- **[M] `read(reg, CM_GPnCTL_Type)` returns uninitialised `value` on default.**
  `CLOCK::control_type value;` is declared without init and only set inside known
  `case`s (`-Wsometimes-uninitialized`).
- **[L] `(~(~0U<<4)) & value | (...)` precedence warning** (`-Wbitwise-op-
  parentheses`) in the three `CM_GPnCTL` `ALL` cases; behaviour is intended but
  the `auto all_value = 0;` makes it `int`.

### 2.4 Interrupt (`src/interrupt/interrupt.cpp`) — ✅ FIXED (handler/enable logic)
> Resolved: `enable`/`disable` now select bank 1 for IRQ 0–31 and bank 2 (shift
> `n-32`) for 32–63; `isEnabled` reads back the Enable register without
> side-effects and with correct `&`/`==` precedence; `install_IRQHandler`/
> `install_FIQHandler` assign the IVT slot directly instead of dereferencing a
> wild pointer. Regression tests added in `test/interrupt_test.cpp`. The IVT
> placement note below (`[M]`) is a design observation left as-is.

- **[H] enable/disable register selection inverted and out of range.**
  ```cpp
  if (irqNumber > 31) Enable_IRQs_1 |= 1U << irqNumber;        // wrong reg + shift>=32 UB
  else                Enable_IRQs_2 |= 1U << (irqNumber - 32); // wrong reg + underflow
  ```
  IRQs 0–31 belong to `Enable_IRQs_1` (shift `number`), IRQs 32–63 to
  `Enable_IRQs_2` (shift `number-32`). The condition is inverted and both shifts
  are wrong. Same defect in `disable()` and `isEnabled()`.
- **[H] `isEnabled()` mutates a read-only pending register.** It does
  `tmpReg &= (1U << number)` on the `IRQ_Pending` register **reference**, i.e. it
  writes back into the (volatile) register during a query. It also reads
  *pending* to answer an *enabled?* question, which is semantically wrong. Plus
  `(tmpReg >> number) & 1U == 1U` has a precedence bug (`==` binds before `&`).
- **[H] `install_IRQHandler` dereferences a wild pointer.**
  `IVT& theIVT = *reinterpret_cast<IVT*>(m_ivt[IVT::Number::Reset]);` casts a
  *function pointer slot value* (initially garbage/null) to an `IVT*` and
  dereferences it. It also ignores the `IRQNumber` argument. It should simply do
  `m_ivt[IVT::Number::IRQ] = cb;`. Same in `install_FIQHandler`.
- **[M] IVT placement is inconsistent / unrealistic.** The default ctor places
  the IVT at `0x00000000`; the region ctor places it right after the IRQ
  registers. On ARM the exception vectors live at the VBAR-relative base, not
  adjacent to peripheral MMIO.

---

## 3. I2C/SPI — shipped, and how they avoid the bugs

The I2C (BSC1) and SPI0 drivers (`inc/i2c.hpp`, `inc/spi.hpp`, `src/i2c`,
`src/spi`, with gtests in `test/`) reuse the exact framework pattern
(register-block struct + placement `new` + driver class + gtest-over-vector), but:

- Index registers **directly by datasheet word offset** — no pin-number
  indirection (avoids 2.2).
- Use correct physical bases: **BSC1 `0x3F804000`**, **SPI0 `0x3F204000`**.
- Provide field-level `set/clr/get` with one consistent bit layout (avoids 2.3).
- Keep read accessors side-effect free and initialise return values (avoids 2.4).

> Note: like the existing drivers these are **host-side bit-layout models**; real
> transfers additionally need correct GPIO ALT0 muxing, clock divider setup, and
> the build/toolchain fixes in 2.1 before they run on a Pi.

The shipped `Bcm2837I2cTransport` (`inc/i2c_bus.hpp`) is **poll-based** (bounded
spin on the status register); the BSC `INTD`/`INTT`/`INTR` interrupt-enable bits
are addressable but unused. An **interrupt-driven** bare-metal variant is
designed in [`i2c-irq-transport-spec.md`](i2c-irq-transport-spec.md) (not yet
implemented). On Linux the interrupt-driven path is already provided by the
kernel via `inc/i2c_dev.hpp` (`/dev/i2c-1`).
