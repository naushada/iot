#ifndef __i2c_bus_test_hpp__
#define __i2c_bus_test_hpp__

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "i2c.hpp"
#include "gpio.hpp"
#include "i2c_bus.hpp"

/**
 * @brief Bcm2837I2cTransport driven over a scripted Status register.
 *
 * The BSC Status bits (DONE/ERR/CLKT/TXD/RXD) are W1C/event bits that a plain
 * RW test buffer cannot emulate, so the transport exposes poll_status() as the
 * seam. This subclass returns a caller-supplied sequence of status words and,
 * whenever it reports S.RXD, stages the next `rx` byte into the FIFO so the
 * read path picks it up — i.e. it plays the role of the slave on the host.
 */
class ScriptedTransport : public Bcm2837I2cTransport {
    public:
        using Bcm2837I2cTransport::Bcm2837I2cTransport;

        std::vector<std::uint32_t> script;  ///< status word returned per poll
        std::vector<std::uint8_t>  rx;      ///< bytes fed when S.RXD is reported
        std::size_t scriptIdx = 0;
        std::size_t rxIdx = 0;

    protected:
        std::uint32_t poll_status() override {
            const std::uint32_t s = (scriptIdx < script.size())
                                        ? script[scriptIdx++]
                                        : (script.empty() ? 0U : script.back());
            if ((s & (1U << 5)) && rxIdx < rx.size()) {   // S.RXD -> stage a byte
                i2c().write_byte(rx[rxIdx++]);
            }
            return s;
        }
};

class I2cBusTest : public ::testing::Test {
    public:
        I2cBusTest()
            : m_i2c_region(BCM2837::BSCRegistersAddress::Register::BSC_MAX),
              m_gpio_region(BCM2837::GPIORegistersAddress::BCM2837_MAX),
              m_i2c(m_i2c_region.data()),
              m_gpio(m_gpio_region.data()) {}

        /// @brief Status bit masks, mirrored from i2c_bus.cpp for the scripts.
        static constexpr std::uint32_t DONE = 1U << 1;
        static constexpr std::uint32_t TXD  = 1U << 4;
        static constexpr std::uint32_t RXD  = 1U << 5;
        static constexpr std::uint32_t ERR  = 1U << 8;
        static constexpr std::uint32_t CLKT = 1U << 9;

        I2C&  i2c()  { return m_i2c; }
        GPIO& gpio() { return m_gpio; }
        std::uint32_t gpfsel0() const { return m_gpio_region[0]; }

    protected:
        std::vector<std::uint32_t> m_i2c_region;
        std::vector<std::uint32_t> m_gpio_region;
        I2C  m_i2c;
        GPIO m_gpio;
};

#endif /*__i2c_bus_test_hpp__*/
