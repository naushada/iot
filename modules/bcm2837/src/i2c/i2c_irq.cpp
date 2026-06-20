#ifndef __i2c_irq_cpp__
#define __i2c_irq_cpp__

#include "i2c_irq.hpp"
#include "system_timer.hpp"

#include <atomic>

namespace {
    /* BSC Status (S) register bit masks — see memory_map.hpp / i2c_bus.cpp. */
    constexpr std::uint32_t S_DONE = 1U << 1;
    constexpr std::uint32_t S_TXD  = 1U << 4;   ///< FIFO can accept data
    constexpr std::uint32_t S_RXD  = 1U << 5;   ///< FIFO contains data
    constexpr std::uint32_t S_ERR  = 1U << 8;   ///< slave NACK
    constexpr std::uint32_t S_CLKT = 1U << 9;   ///< clock-stretch timeout

    constexpr std::uint8_t  ADDR_MAX = 0x7F;

    using C = BCM2837::BSCRegistersAddress::Control;
    using S = BCM2837::BSCRegistersAddress::Status;

    /// Cross-context (ISR ↔ thread) + cross-peripheral ordering. Real DMB on
    /// ARM (safe in userspace); a compiler/seq-cst fence elsewhere (host tests).
    inline void mem_barrier() {
#if defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("dmb sy" ::: "memory");
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    }

    /// The active instance the static IRQ trampoline dispatches to (one BSC
    /// controller ⇒ one instance services its line).
    Bcm2837I2cIrqTransport* s_instance = nullptr;

    /// Installed via IRQ::install_IRQHandler (which takes a bare void(*)()).
    void bsc_isr_trampoline() {
        if (s_instance != nullptr) s_instance->handle_irq();
    }
}

void Bcm2837I2cIrqTransport::bus_init() {
    m_gpio.write(2, BCM2837::GPIORegistersAddress::Config::AlternateFunction0);
    m_gpio.write(3, BCM2837::GPIORegistersAddress::Config::AlternateFunction0);
    m_i2c.clock_divider(m_divider);
    m_i2c.enable();
    s_instance = this;
    m_irq.install_IRQHandler(kBsc1Irq, &bsc_isr_trampoline);
    m_irq.enable(kBsc1Irq);
    mem_barrier();
}

std::uint32_t Bcm2837I2cIrqTransport::read_status() {
    return m_i2c.memory().m_register[BCM2837::BSCRegistersAddress::Register::S];
}

void Bcm2837I2cIrqTransport::wait_for_irq() {
#if defined(I2C_IRQ_BAREMETAL) && (defined(__aarch64__) || defined(__arm__))
    __asm__ volatile("wfi");   // sleep until an interrupt; the ISR sets done.
#endif
    // Host/default: no-op. The watchdog loop in wait_complete() bounds the wait;
    // tests override this to drive handle_irq(). A real bare-metal build must
    // also arm a timer IRQ so WFI wakes for the watchdog (spec §4.7).
}

std::uint64_t Bcm2837I2cIrqTransport::now_us() {
    return m_timer != nullptr ? m_timer->now_us() : 0;
}

I2cResult Bcm2837I2cIrqTransport::start(std::uint8_t addr,
                                        const std::uint8_t* tx, std::size_t tn,
                                        std::uint8_t* rx, std::size_t rn,
                                        bool reading, Completion cb, void* user) {
    m_xfer = Xfer{};
    m_xfer.tx = tx; m_xfer.tn = tn;
    m_xfer.rx = rx; m_xfer.rn = rn;
    m_xfer.reading = reading;
    m_xfer.cb = cb; m_xfer.user = user;
    m_xfer.active = true;

    m_i2c.clear_fifo();
    m_i2c.clr_status(S::DONE);
    m_i2c.clr_status(S::ERR);
    m_i2c.clr_status(S::CLKT_TO);
    m_i2c.slave_address(addr);
    m_i2c.data_length(static_cast<I2C::value_type>(reading ? rn : tn));

    if (reading) {
        m_i2c.set_control(C::INTR, 1);   // RX FIFO needs reading
        m_i2c.set_control(C::INTD, 1);   // transfer done
        m_i2c.start_read();
    } else {
        m_i2c.set_control(C::INTT, 1);   // TX FIFO needs writing
        m_i2c.set_control(C::INTD, 1);
        m_i2c.start_write();             // empty FIFO + TA → TXW fires → ISR fills
    }
    mem_barrier();
    return I2cResult::Ok;
}

void Bcm2837I2cIrqTransport::handle_irq() {
    mem_barrier();
    for (;;) {
        const std::uint32_t s = read_status();
        if (s & S_ERR)  { finalize(I2cResult::Nack);         return; }
        if (s & S_CLKT) { finalize(I2cResult::ClockTimeout); return; }

        if (m_xfer.reading) {
            if ((s & S_RXD) && m_xfer.ri < m_xfer.rn) {
                m_xfer.rx[m_xfer.ri++] =
                    static_cast<std::uint8_t>(m_i2c.read_byte() & 0xFFU);
                continue;
            }
        } else {
            if ((s & S_TXD) && m_xfer.ti < m_xfer.tn) {
                m_i2c.write_byte(m_xfer.tx[m_xfer.ti++]);
                continue;
            }
        }

        if (s & S_DONE) {
            const bool ok = m_xfer.reading ? (m_xfer.ri == m_xfer.rn) : true;
            finalize(ok ? I2cResult::Ok : I2cResult::Timeout);
            return;
        }
        return;   // nothing to move and not done — wait for the next interrupt
    }
}

void Bcm2837I2cIrqTransport::finalize(I2cResult r) {
    m_i2c.clr_status(S::DONE);
    m_i2c.clr_status(S::ERR);
    m_i2c.clr_status(S::CLKT_TO);
    m_i2c.clr_control(C::INTD);
    m_i2c.clr_control(C::INTT);
    m_i2c.clr_control(C::INTR);
    m_xfer.result = r;
    mem_barrier();
    m_xfer.done = true;
    m_xfer.active = false;
    if (m_xfer.cb != nullptr) {
        const Completion cb = m_xfer.cb;
        void* const user = m_xfer.user;
        m_xfer.cb = nullptr;
        cb(r, user);
    }
}

void Bcm2837I2cIrqTransport::abort_transfer() {
    m_i2c.clr_control(C::INTD);
    m_i2c.clr_control(C::INTT);
    m_i2c.clr_control(C::INTR);
    m_i2c.clear_fifo();
    m_i2c.clr_status(S::DONE);
    m_i2c.clr_status(S::ERR);
    m_i2c.clr_status(S::CLKT_TO);
    m_xfer.active = false;
}

I2cResult Bcm2837I2cIrqTransport::wait_complete() {
    // Bound the wait two ways: a wall-clock µs deadline (the real watchdog, when
    // a time source is wired) and an absolute iteration cap (backstop for the
    // host / no-clock builds). Whichever trips first aborts the transfer.
    const std::uint64_t start = now_us();
    for (std::uint32_t i = 0; i < m_max_waits; ++i) {
        mem_barrier();
        if (m_xfer.done) {
            return m_xfer.result;
        }
        if (m_timeout_us != 0) {
            const std::uint64_t t = now_us();
            // now_us()==0 means no time source is wired → skip the µs check and
            // let the iteration cap govern; otherwise abort once N µs elapse.
            if (t != 0 && (t - start) >= m_timeout_us) {
                break;
            }
        }
        wait_for_irq();
    }
    abort_transfer();    // wedged bus / lost interrupt / deadline exceeded
    return I2cResult::Timeout;
}

I2cResult Bcm2837I2cIrqTransport::write(std::uint8_t addr,
                                        const std::uint8_t* buf, std::size_t len) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX) return I2cResult::BadArg;
    start(addr, buf, len, nullptr, 0, /*reading=*/false, nullptr, nullptr);
    return wait_complete();
}

I2cResult Bcm2837I2cIrqTransport::read(std::uint8_t addr,
                                       std::uint8_t* buf, std::size_t len) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX) return I2cResult::BadArg;
    start(addr, nullptr, 0, buf, len, /*reading=*/true, nullptr, nullptr);
    return wait_complete();
}

I2cResult Bcm2837I2cIrqTransport::write_read(std::uint8_t addr,
                                             const std::uint8_t* wbuf, std::size_t wlen,
                                             std::uint8_t* rbuf, std::size_t rlen) {
    const I2cResult w = write(addr, wbuf, wlen);   // BSC has no repeated-START
    if (w != I2cResult::Ok) return w;
    return read(addr, rbuf, rlen);
}

I2cResult Bcm2837I2cIrqTransport::write_async(std::uint8_t addr,
                                              const std::uint8_t* buf, std::size_t len,
                                              Completion cb, void* user) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX || m_xfer.active) {
        return I2cResult::BadArg;
    }
    return start(addr, buf, len, nullptr, 0, /*reading=*/false, cb, user);
}

I2cResult Bcm2837I2cIrqTransport::read_async(std::uint8_t addr,
                                             std::uint8_t* buf, std::size_t len,
                                             Completion cb, void* user) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX || m_xfer.active) {
        return I2cResult::BadArg;
    }
    return start(addr, nullptr, 0, buf, len, /*reading=*/true, cb, user);
}

#endif /*__i2c_irq_cpp__*/
