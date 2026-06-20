#ifndef __opt3001_cpp__
#define __opt3001_cpp__

#include "opt3001.hpp"

bool Opt3001::read_u16(std::uint8_t reg, std::uint16_t& out) {
    std::uint8_t b[2] = {0};
    if (read_regs(reg, b, sizeof(b)) != I2cResult::Ok) {
        return false;
    }
    /* OPT3001 is MSB-first. */
    out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(b[0]) << 8) | b[1]);
    return true;
}

I2cResult Opt3001::write_u16(std::uint8_t reg, std::uint16_t val) {
    const std::uint8_t b[3] = {
        reg,
        static_cast<std::uint8_t>(val >> 8),
        static_cast<std::uint8_t>(val & 0xFF),
    };
    return m_bus.write(m_addr, b, sizeof(b));
}

bool Opt3001::probe() {
    std::uint16_t id = 0;
    return read_u16(kRegDeviceId, id) && id == kDeviceId;
}

I2cResult Opt3001::init() {
    return write_u16(kRegConfig, kConfigContinuous);
}

bool Opt3001::read_lux(double& lux) {
    std::uint16_t raw = 0;
    if (!read_u16(kRegResult, raw)) {
        return false;
    }
    const std::uint32_t exponent = (raw >> 12) & 0x0F;
    const std::uint32_t mantissa = raw & 0x0FFF;
    /* lux = 0.01 · 2^E · mantissa (datasheet §7.3.3). */
    lux = 0.01 * static_cast<double>(1U << exponent) * static_cast<double>(mantissa);
    return true;
}

#endif /*__opt3001_cpp__*/
