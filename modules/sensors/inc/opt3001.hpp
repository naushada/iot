#ifndef __opt3001_hpp__
#define __opt3001_hpp__

#include <cstdint>

#include "i2c_sensor.hpp"

/**
 * @file opt3001.hpp
 * @brief TI OPT3001 ambient light sensor (mangOH Yellow light sensor).
 *
 * NOTE: the exact mangOH Yellow light-sensor part is to be confirmed on
 * hardware (TDD open question #1); OPT3001 at 0x44 is the typical fit and the
 * default here. OPT3001 registers are 16-bit, MSB-first. The Result register
 * (0x00) is a 4-bit exponent + 12-bit mantissa; lux = 0.01 · 2^E · mantissa.
 */
class Opt3001 : public I2cSensor {
    public:
        static constexpr std::uint8_t  kAddr     = 0x44;
        static constexpr std::uint8_t  kRegResult = 0x00;
        static constexpr std::uint8_t  kRegConfig = 0x01;
        static constexpr std::uint8_t  kRegDeviceId = 0x7F;
        static constexpr std::uint16_t kDeviceId  = 0x3001;
        /// @brief Config: auto full-scale range, 800 ms, continuous conversion.
        static constexpr std::uint16_t kConfigContinuous = 0xCE10;

        explicit Opt3001(I2cTransport& bus, std::uint8_t addr = kAddr)
            : I2cSensor(bus, addr) {}

        /// @brief True if the Device-ID register reads back 0x3001.
        bool probe();
        /// @brief Start continuous, auto-ranged conversion. 0 == Ok.
        I2cResult init();
        /// @brief Read the Result register and decode to lux. False on bus error.
        bool read_lux(double& lux);

    private:
        /// @brief Read a big-endian 16-bit register.
        bool read_u16(std::uint8_t reg, std::uint16_t& out);
        /// @brief Write a big-endian 16-bit register.
        I2cResult write_u16(std::uint8_t reg, std::uint16_t val);
};

#endif /*__opt3001_hpp__*/
