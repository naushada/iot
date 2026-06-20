#ifndef __bmi160_cpp__
#define __bmi160_cpp__

#include "bmi160.hpp"

namespace {
    /* BMI160 register map (datasheet §2.11). */
    constexpr std::uint8_t REG_CHIP_ID = 0x00;
    constexpr std::uint8_t REG_DATA    = 0x0C;  /* GYR_X_L; 12 bytes: gyro then accel */
    constexpr std::uint8_t REG_CMD     = 0x7E;

    constexpr std::uint8_t CMD_ACC_NORMAL = 0x11;  /* set accel  PMU -> normal */
    constexpr std::uint8_t CMD_GYR_NORMAL = 0x15;  /* set gyro   PMU -> normal */

    /// @brief Assemble a little-endian signed 16-bit sample.
    std::int16_t le16(std::uint8_t lo, std::uint8_t hi) {
        return static_cast<std::int16_t>(
            static_cast<std::uint16_t>(lo) | (static_cast<std::uint16_t>(hi) << 8));
    }
}

bool Bmi160::probe() {
    std::uint8_t id = 0;
    return read_u8(REG_CHIP_ID, id) && id == kChipId;
}

I2cResult Bmi160::init() {
    /* Order matters per the datasheet: accel first, then gyro. On hardware each
       command needs a few ms to settle; the sampler re-reads on a timer so a
       transient not-yet-ready read is simply retried. */
    const I2cResult r = write_u8(REG_CMD, CMD_ACC_NORMAL);
    if (r != I2cResult::Ok) {
        return r;
    }
    return write_u8(REG_CMD, CMD_GYR_NORMAL);
}

bool Bmi160::read(Sample& out) {
    std::uint8_t b[12] = {0};
    if (read_regs(REG_DATA, b, sizeof(b)) != I2cResult::Ok) {
        return false;
    }
    out.gx = le16(b[0],  b[1]);
    out.gy = le16(b[2],  b[3]);
    out.gz = le16(b[4],  b[5]);
    out.ax = le16(b[6],  b[7]);
    out.ay = le16(b[8],  b[9]);
    out.az = le16(b[10], b[11]);
    return true;
}

#endif /*__bmi160_cpp__*/
