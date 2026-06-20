#ifndef __i2c_irq_hpp__
#define __i2c_irq_hpp__

#include <cstdint>
#include <cstddef>

#include "i2c.hpp"
#include "gpio.hpp"
#include "interrupt.hpp"
#include "i2c_bus.hpp"   // I2cTransport / I2cResult

/**
 * @file i2c_irq.hpp
 * @brief Interrupt-driven BSC1 I²C transport (bare-metal). See
 *        docs/i2c-irq-transport-spec.md.
 *
 * Same `I2cTransport` seam as the polling Bcm2837I2cTransport, so it is a
 * drop-in for the sensor drivers and host-unit-testable the same way (simulate
 * the ISR over a vector-backed register block). The transfer is driven by the
 * BSC DONE/TXW/RXR interrupts instead of a status-register spin:
 *
 *   start() programs A/DLEN, enables the INT* bits, kicks ST, then blocks in
 *   wait_complete(); the installed ISR (handle_irq) moves FIFO bytes and
 *   finalizes on DONE/ERR/CLKT; a watchdog bounds the wait for a wedged bus.
 *
 * BARE-METAL ONLY. On Linux use I2cDevTransport (/dev/i2c-1) — the kernel
 * already drives the BSC by interrupt; a userspace ISR cannot coexist with it.
 */
class Bcm2837I2cIrqTransport : public I2cTransport {
    public:
        /// ~100 kHz from a 250 MHz core clock (tune for clock-stretch slaves).
        static constexpr std::uint16_t kDefaultDivider  = 2500;
        /// BSC interrupt line — BCM2837 DT `interrupts = <2 21>` ⇒ IRQ 53.
        /// CONFIRM on silicon (shared across BSC0/1/2). See spec §3.
        static constexpr std::uint8_t  kBsc1Irq         = 53;
        /// Watchdog: max wait_for_irq() iterations before aborting → Timeout.
        static constexpr std::uint32_t kDefaultMaxWaits = 1000000U;

        /// Async completion callback (no captures — pass state via `user`).
        using Completion = void (*)(I2cResult result, void* user);

        Bcm2837I2cIrqTransport(I2C i2c, GPIO gpio, IRQ irq,
                               std::uint16_t divider   = kDefaultDivider,
                               std::uint32_t max_waits = kDefaultMaxWaits)
            : m_i2c(i2c), m_gpio(gpio), m_irq(irq),
              m_divider(divider), m_max_waits(max_waits) {}

        /// Mux GPIO2/3 → ALT0, set the divider, enable BSC1, install the ISR and
        /// enable the BSC IRQ line. Call once before any transfer (hardware).
        void bus_init();

        /* ---- I2cTransport (synchronous: start + block) ---- */
        I2cResult write(std::uint8_t addr,
                        const std::uint8_t* buf, std::size_t len) override;
        I2cResult read(std::uint8_t addr,
                       std::uint8_t* buf, std::size_t len) override;
        I2cResult write_read(std::uint8_t addr,
                             const std::uint8_t* wbuf, std::size_t wlen,
                             std::uint8_t* rbuf, std::size_t rlen) override;

        /* ---- P5: async (kick + return; the ISR fires `cb`) ---- */
        /// Begin a write and return immediately (Ok = in flight). `cb` is
        /// invoked from the ISR on completion. Returns BadArg on bad args or if
        /// a transfer is already in flight (one controller, one transfer).
        I2cResult write_async(std::uint8_t addr, const std::uint8_t* buf,
                              std::size_t len, Completion cb, void* user);
        I2cResult read_async(std::uint8_t addr, std::uint8_t* buf,
                             std::size_t len, Completion cb, void* user);

        /// ISR body: move available FIFO bytes, finalize on DONE/ERR/CLKT.
        /// Public so the static trampoline and host tests can invoke it.
        void handle_irq();

        I2C&  i2c()  { return m_i2c; }
        GPIO& gpio() { return m_gpio; }
        IRQ&  irq()  { return m_irq; }

    protected:
        /// Live Status (S) register snapshot. Virtual: tests script it and stage
        /// RX bytes; production reads the register.
        virtual std::uint32_t read_status();
        /// Block the caller until the next interrupt. Production: WFI (with
        /// I2C_IRQ_BAREMETAL) else a no-op; tests override to drive handle_irq().
        virtual void wait_for_irq();

    private:
        struct Xfer {
            const std::uint8_t* tx = nullptr; std::size_t tn = 0, ti = 0;
            std::uint8_t*       rx = nullptr; std::size_t rn = 0, ri = 0;
            bool                reading = false;
            volatile bool       done    = false;
            volatile I2cResult  result  = I2cResult::Timeout;
            Completion          cb      = nullptr;
            void*               user    = nullptr;
            bool                active  = false;
        };

        I2cResult start(std::uint8_t addr,
                        const std::uint8_t* tx, std::size_t tn,
                        std::uint8_t* rx, std::size_t rn, bool reading,
                        Completion cb, void* user);
        I2cResult wait_complete();      ///< watchdog loop over wait_for_irq()
        void      finalize(I2cResult r);
        void      abort_transfer();

        I2C           m_i2c;
        GPIO          m_gpio;
        IRQ           m_irq;
        std::uint16_t m_divider;
        std::uint32_t m_max_waits;
        Xfer          m_xfer;
};

#endif /*__i2c_irq_hpp__*/
