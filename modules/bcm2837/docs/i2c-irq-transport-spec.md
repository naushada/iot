# Spec — Interrupt-driven BSC1 I²C transport (`Bcm2837I2cIrqTransport`)

Status: **IMPLEMENTED (P1–P5)** in `inc/i2c_irq.hpp` + `src/i2c/i2c_irq.cpp`,
host-tested in `test/i2c_irq_test.{hpp,cpp}` (11 gtests — DONE path, multi-
interrupt drain, watchdog, error classes, bus_init/IRQ-arm, async callback).
A bare-metal follow-up to the polling `Bcm2837I2cTransport` (`inc/i2c_bus.hpp`)
on the same `I2cTransport` seam — a drop-in for the sensor drivers, host-unit-
testable by simulating the ISR. **On-silicon validation (P4) is pending real
hardware** (the `kBsc1Irq`=53 line, IVT wiring, barriers, clock-stretch divider).

---

## 1. Goal & scope

Replace the busy-wait spin in `Bcm2837I2cTransport` (the `for (spin < m_max_spins)`
loop on `poll_status()`) with a transfer driven by the **BSC completion / FIFO
interrupts**, so the CPU does not burn cycles polling the status register during
a transfer.

**In scope:** a freestanding / bare-metal-or-RTOS build that owns the
interrupt-vector and the BSC IRQ line (the bcm2837 `IRQ` driver in
`inc/interrupt.hpp`).

**Explicitly OUT of scope:** Linux. On a booted device the shipping path is
`I2cDevTransport` (`/dev/i2c-1`), where the kernel `i2c_bcm2835` driver is
*already* interrupt-driven, arbitrated and multi-master-safe. **Do not use this
transport under Linux** — `/dev/mem` register poking + a userspace ISR cannot
coexist with the kernel driver. This spec targets the no-Linux consumer only.

**Non-goals:** multi-master arbitration, 10-bit addressing.

> ⚠️ **DMA is NOT available for the BSC I²C masters on BCM2837** (an earlier
> draft of this note wrongly called it "a separate effort"). The BSC0/1/2 I²C
> masters have **no DMA DREQ line**, so a DMA channel cannot be paced against the
> 16-byte FIFO — it would overrun immediately. Corroborated by this module's own
> register model: `BSCRegistersAddress::Control` (`memory_map.hpp`) has no
> `DMAEN` bit, whereas `SPIRegistersAddress::ControlStatus` does (bit 8, plus the
> `DC` DMA-DREQ-controls register). In the BCM2835 DREQ table, entries 8/9
> ("BSC/SPI Slave") map to the BSC **slave** peripheral (`0x7E214000`), not the
> I²C masters. This is why mainline Linux `i2c-bcm2835` is PIO/interrupt-driven
> only. **The interrupt-driven transport (this spec) is the CPU-offload ceiling
> for BSC I²C.** SPI0 *does* have DMA (DREQ 6/7) — a DMA transport is viable there
> if a future SPI peripheral ever needs it, but no mangOH Yellow sensor is SPI.

---

## 2. Why (when polling is not enough)

The poll transport is fine for the 10 s sensor cadence. Interrupt-driving the
register path matters when:

- a slave **clock-stretches** (BME680 can) — the poll loop spins the whole time;
- the core has **other work** to do during a transfer (an RTOS / event loop);
- **power** matters — the core can `WFI` between interrupts instead of spinning;
- transfers are **larger than the 16-byte FIFO** and need refill/drain mid-flight.

---

## 3. Hardware background (BSC1)

Register model already in `memory_map.hpp` / `i2c.hpp`:

- **Control (C)** interrupt-enable bits (already addressable via `set_control`):
  - `INTD` (bit 8) — interrupt on **DONE**.
  - `INTT` (bit 9) — interrupt on **TXW** (TX FIFO needs writing / below watermark).
  - `INTR` (bit 10) — interrupt on **RXR** (RX FIFO needs reading / above watermark).
- **Status (S)** bits the ISR reads: `DONE` (W1C), `TXW`, `RXR`, `TXD`, `RXD`,
  `TXE`, `RXF`, `ERR` (NACK, W1C), `CLKT_TO` (clock-stretch timeout, W1C), `TA`.
- **FIFO** is **16 bytes** deep. `DLEN` programs the byte count; `A` the address.

> ⚠️ **Open constant — BSC1 IRQ number.** The BSC master interrupt routes
> through the BCM2837 *GPU* IRQ block, not a plain ARM IRQ; the exact line must
> be read from the datasheet peripheral-IRQ table / the kernel `bcm2835-i2c` DT
> `interrupts` property and confirmed on silicon. Call it `kBsc1Irq` — a
> required, to-be-confirmed input to `IRQ::enable()` / `install_IRQHandler()`.

> ⚠️ **BCM2835/2837 I²C clock-stretch erratum.** The BSC has a documented bug
> where a slave that stretches the clock *immediately* after the SCL falling
> edge of the first bit can corrupt the read. Mitigation: keep SCL ≤ 100 kHz for
> stretch-prone slaves (BME680), or gate forced-mode reads so the device is
> ready. The interrupt model does not fix this — it's a bus-timing issue —
> document it so the integrator picks the divider accordingly.

> ⚠️ **Peripheral memory barriers.** Per the BCM2835 datasheet §1.3, accesses to
> *different* peripherals can be reordered; the ISR and the thread both touch
> BSC MMIO, and the ISR entry/exit crosses into the IRQ controller. Put a `DMB`
> at ISR entry and before the completion store, and after enabling the BSC IRQ.

---

## 4. Architecture

### 4.1 Class
`Bcm2837I2cIrqTransport : public I2cTransport`, constructed like the poll
transport (over an `I2C` + `GPIO` handle) plus an `IRQ` handle:

```cpp
class Bcm2837I2cIrqTransport : public I2cTransport {
  public:
    Bcm2837I2cIrqTransport(I2C i2c, GPIO gpio, IRQ irq,
                           std::uint16_t divider = 2500);
    void bus_init();        // ALT0 mux + divider + enable + install ISR + IRQ on
    I2cResult write(...)       override;   // blocking; ISR does the work
    I2cResult read(...)        override;
    I2cResult write_read(...)  override;   // BSC has no repeated-START → 2 xfers
  protected:
    virtual void wait_complete();   // test seam: how the caller blocks
  ...
};
```

### 4.2 Transfer context (ISR ↔ thread shared state)
One controller ⇒ one in-flight transfer. The ISR has no argument
(`IVT::pointerToFn` is `void(*)()`), so the active transfer is reached through a
**file-static pointer**:

```cpp
struct Xfer {
    const std::uint8_t* tx; std::size_t tn; std::size_t ti;   // write side
    std::uint8_t*       rx; std::size_t rn; std::size_t ri;   // read side
    volatile I2cResult  result;
    volatile bool       done;     // set by ISR, observed by wait_complete()
};
static volatile Xfer*  g_active = nullptr;   // guarded: one transfer at a time
static Bcm2837I2cIrqTransport* g_self = nullptr;  // for register access in the ISR
```

`g_active` / `done` / `result` are `volatile` and crossed with a `DMB`; the
single-controller invariant means no lock is needed beyond a busy guard
(reject/return `BadArg`-like "Busy" if a second transfer starts).

### 4.3 State machine (driven by the ISR)
```
 IDLE ──start_write──▶ WRITING ──(TXW: refill FIFO)──▶ ... ──(DONE)──▶ COMPLETE
 IDLE ──start_read───▶ READING ──(RXR: drain FIFO)───▶ ... ──(DONE)──▶ COMPLETE
                         │
                         └──(ERR → Nack | CLKT → ClockTimeout)──▶ COMPLETE(error)
```

### 4.4 Two FIFO paths
- **Small transfer (≤ 16 bytes — every sensor reg read, IMU 12-byte burst,
  BME680 8-byte field):** preload the whole TX FIFO before `ST`, enable **`INTD`
  only**, and let the single DONE interrupt finalize (drain RX in the ISR). This
  is the common, simplest path.
- **Large transfer (> 16 bytes):** also enable `INTT` (write) / `INTR` (read);
  the ISR refills on `TXW` / drains on `RXR` until `DONE`. Needed only for bulk
  reads bigger than the FIFO.

### 4.5 ISR (`static void i2c_isr()`)
Fast, allocation-free:
```
DMB
s = S register (one read)
if (s & ERR)  { result = Nack;         finish(); return; }
if (s & CLKT) { result = ClockTimeout; finish(); return; }
if (WRITING && (s & TXW))  while (ti < tn && (S & TXD)) write_byte(tx[ti++]);
if (READING && (s & RXR))  while (ri < rn && (S & RXD)) rx[ri++] = read_byte();
if (s & DONE) {
    if (READING) while (ri < rn && (S & RXD)) rx[ri++] = read_byte();  // tail
    result = (ri==rn || !READING) ? Ok : Timeout;
    finish();
}
// finish(): clr_status(DONE|ERR|CLKT); clr_control(INTD|INTT|INTR);
//           g_active->done = true; DMB;
```

### 4.6 Completion (`wait_complete`)
Synchronous `I2cTransport` ⇒ the thread blocks after kicking the transfer:
- **bare-metal default:** `while (!ctx.done) { __asm__("wfi"); }` — the core
  sleeps until *an* interrupt (then re-checks). Strictly lower power than the
  poll transport's spin.
- **RTOS variant:** `wait_complete()` is `virtual`; an RTOS subclass blocks on a
  binary semaphore the ISR `give`s. (Also the **test seam** — see §6.)
- **async variant (optional, separate API):** return immediately; the ISR
  invokes a `std::function<void(I2cResult)>` completion callback. Does *not* fit
  the synchronous `I2cTransport` signature, so it would be an additive method,
  not an override.

### 4.7 Watchdog (mandatory)
A wedged slave (holds SCL low, never raises DONE) yields **no interrupt** → the
thread would `WFI` forever. `wait_complete()` must bound the wait:
- coarse: a fallback `ST`-to-now spin/iteration cap that aborts → `Timeout`
  (resets the controller: `clear_fifo()`, `clr_status(DONE|ERR|CLKT)`);
- better: arm the ARM system timer for `N ms` and abort on expiry. (The bcm2837
  module has no system-timer driver yet — a small dependency to add, or inject a
  `now()`/`deadline` hook.)

---

## 5. `bus_init()` sequence
1. Mux GPIO2/3 → ALT0, program `clock_divider`, `enable()` (as the poll transport).
2. `clr_status(DONE|ERR|CLKT)`, `clear_fifo()`.
3. `irq.install_IRQHandler(kBsc1Irq, &i2c_isr)`; set `g_self = this`.
4. `irq.enable(kBsc1Irq)`; `DMB`.
Per transfer: set `A`/`DLEN`, preload FIFO (small path), set `g_active`, enable
the needed `INT*` bits, `start_write()/start_read()`, then `wait_complete()`.

---

## 6. Host testability (keep the gtest model)
The register state machine is unit-tested exactly like the poll transport's
scripted `poll_status()` seam:
- construct over `std::vector`-backed `I2C`/`GPIO` blocks;
- override `wait_complete()` in a test subclass to **drive the ISR manually** —
  seed the buffer's `S` register (TXW/RXR/DONE/ERR/CLKT), call `i2c_isr()`, and
  loop until `done`, simulating the controller;
- assert: FIFO byte sequence, `A`/`DLEN`/`INT*` programming, and the mapped
  `I2cResult` (Ok / Nack / ClockTimeout / Timeout).
No hardware, no real interrupts — same approach that gives the poll transport
its 12 green tests. Target a comparable suite (small-write DONE-only, large-write
TXW refill, read RXR drain + DONE tail, ERR→Nack, CLKT→ClockTimeout, watchdog→
Timeout, busy-guard rejects re-entry).

---

## 7. Integration & migration
- Drop-in `I2cTransport` ⇒ `sample_all()` / the sensor drivers are unchanged.
- **Not** wired into `iot-sensord` (Linux → i2c-dev). It's selected by the
  bare-metal consumer that constructs the transport directly.
- The poll transport stays as the simplest model + the no-IRQ fallback.

---

## 8. Risks / open questions
1. **`kBsc1Irq`** — exact BSC1 IRQ line (GPU IRQ block) — confirm from datasheet
   / DT before anything works.
2. **FIFO watermark thresholds** — confirm the exact TXW/RXR trigger levels (and
   whether `DEL`/control configures them) for the large-transfer path.
3. **Clock-stretch erratum** — pick the divider for stretch-prone slaves; the
   ISR can't paper over a bus-timing bug.
4. **Timer dependency** — the watchdog needs a time source the module doesn't
   ship yet (system timer driver or an injected deadline hook).
5. **`install_IRQHandler` IVT model** — `interrupt.cpp`'s IVT placement is a
   bare-metal-only construct (see DRIVER_REVIEW §2.4); validate the vector wiring
   on real silicon, not just the host model.
6. **Barriers** — verify `DMB` placement (ISR entry, pre-completion store,
   post-IRQ-enable) against the BCM2835 cross-peripheral ordering rule.

---

## 9. Phased plan — status
- **P1 ✅** — `Bcm2837I2cIrqTransport` + `Xfer` ctx + small-transfer DONE path +
  `wait_complete()`/`read_status()` seams; host gtests (ISR simulated).
- **P2 ✅** — multi-interrupt FIFO drain/fill (`handle_irq` moves all available
  bytes per call, re-reading status; the inner loop re-arms across interrupts).
- **P3 ✅** — watchdog: `wait_complete()` bounds the wait by `max_waits`
  `wait_for_irq()` cycles, then `abort_transfer()` → `Timeout`. (A real
  bare-metal build must arm a timer IRQ so `WFI` wakes — §4.7.)
- **P4 ⚙️ code done, silicon pending** — `bus_init()` muxes ALT0, sets the
  divider, enables BSC, `install_IRQHandler(kBsc1Irq, trampoline)` + `enable()`;
  `mem_barrier()` = `dmb sy` on ARM. Confirm `kBsc1Irq`=53, IVT wiring and the
  clock-stretch divider **on hardware**; A/B against the poll transport.
- **P5 ✅** — `write_async`/`read_async` (kick + return; the ISR fires the
  `Completion` callback); a busy guard rejects a second in-flight transfer.

> **Build note:** the bare-metal `WFI` in `wait_for_irq()` is gated behind
> `-DI2C_IRQ_BAREMETAL` (+ ARM) so the hosted/test build is a safe no-op.
