#ifndef __i2c_sensor_hpp__
#define __i2c_sensor_hpp__

#include <cstdint>
#include <cstddef>

#include "i2c_bus.hpp"   // I2cTransport / I2cResult (modules/bcm2837)

/**
 * @file i2c_sensor.hpp
 * @brief Common base for the mangOH Yellow I²C sensor drivers.
 *
 * Each chip driver holds a reference to an `I2cTransport` (the seam from
 * modules/bcm2837) and its 7-bit address, and reads/writes 8-bit device
 * registers through it. Depending only on the abstract transport keeps every
 * driver host-unit-testable against a fake register file — no BSC1, no Pi.
 */
class I2cSensor {
    public:
        I2cSensor(I2cTransport& bus, std::uint8_t addr) : m_bus(bus), m_addr(addr) {}
        virtual ~I2cSensor() = default;

        std::uint8_t address() const { return m_addr; }

    protected:
        /// @brief Read `n` bytes starting at device register `reg`.
        I2cResult read_regs(std::uint8_t reg, std::uint8_t* buf, std::size_t n) {
            return m_bus.read_reg(m_addr, reg, buf, n);
        }
        /// @brief Read a single device register; returns false on bus error.
        bool read_u8(std::uint8_t reg, std::uint8_t& out) {
            return read_regs(reg, &out, 1) == I2cResult::Ok;
        }
        /// @brief Write one byte to device register `reg`.
        I2cResult write_u8(std::uint8_t reg, std::uint8_t val) {
            return m_bus.write_reg(m_addr, reg, val);
        }

        I2cTransport& m_bus;
        std::uint8_t  m_addr;
};

#endif /*__i2c_sensor_hpp__*/
