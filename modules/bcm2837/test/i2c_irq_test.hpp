#ifndef __i2c_irq_test_hpp__
#define __i2c_irq_test_hpp__

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "i2c.hpp"
#include "gpio.hpp"
#include "interrupt.hpp"
#include "i2c_irq.hpp"

/**
 * @brief Bcm2837I2cIrqTransport driven by a scripted ISR on the host.
 *
 * Same idea as the poll transport's ScriptedTransport: `read_status()` returns
 * a caller-supplied sequence of S-register words and stages the next `rx` byte
 * into the FIFO whenever it reports S.RXD, and `wait_for_irq()` drives one
 * handle_irq() (simulating an interrupt firing) — so the whole interrupt path
 * is exercised without real interrupts.
 */
class ScriptedIrqTransport : public Bcm2837I2cIrqTransport {
    public:
        using Bcm2837I2cIrqTransport::Bcm2837I2cIrqTransport;

        std::vector<std::uint32_t> script;   ///< S-register word per read_status()
        std::vector<std::uint8_t>  rx;        ///< bytes fed when S.RXD is reported
        std::size_t scriptIdx = 0;
        std::size_t rxIdx = 0;

    protected:
        std::uint32_t read_status() override {
            const std::uint32_t s = (scriptIdx < script.size())
                                        ? script[scriptIdx++] : 0U;
            if ((s & (1U << 5)) && rxIdx < rx.size()) {   // S.RXD → stage a byte
                i2c().write_byte(rx[rxIdx++]);
            }
            return s;
        }
        void wait_for_irq() override { handle_irq(); }    // simulate the ISR
};

class I2cIrqTest : public ::testing::Test {
    public:
        I2cIrqTest()
            : m_i2cReg(BCM2837::BSCRegistersAddress::Register::BSC_MAX),
              m_gpioReg(BCM2837::GPIORegistersAddress::BCM2837_MAX),
              m_irqReg(1024),
              m_i2c(m_i2cReg.data()),
              m_gpio(m_gpioReg.data()),
              m_irq(m_irqReg.data()) {}

        static constexpr std::uint32_t DONE = 1U << 1;
        static constexpr std::uint32_t TXD  = 1U << 4;
        static constexpr std::uint32_t RXD  = 1U << 5;
        static constexpr std::uint32_t ERR  = 1U << 8;
        static constexpr std::uint32_t CLKT = 1U << 9;

        I2C&  i2c()  { return m_i2c; }
        GPIO& gpio() { return m_gpio; }
        IRQ&  irq()  { return m_irq; }
        std::uint32_t gpfsel0() const { return m_gpioReg[0]; }

    protected:
        std::vector<std::uint32_t> m_i2cReg;
        std::vector<std::uint32_t> m_gpioReg;
        std::vector<std::uint32_t> m_irqReg;
        I2C  m_i2c;
        GPIO m_gpio;
        IRQ  m_irq;
};

#endif /*__i2c_irq_test_hpp__*/
