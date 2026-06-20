#ifndef __bmi160_hpp__
#define __bmi160_hpp__

#include <cstdint>

#include "i2c_sensor.hpp"

/**
 * @file bmi160.hpp
 * @brief Bosch BMI160 6-axis accelerometer + gyroscope (mangOH Yellow IMU).
 *
 * I²C address 0x68 (SDO low) or 0x69 (SDO high). The driver probes the chip-id,
 * brings the accelerometer and gyroscope into normal power mode, and reads the
 * six 16-bit signed axis registers in one burst. Values are raw LSB counts;
 * conversion to m/s² / deg/s depends on the configured ranges and is applied by
 * the caller (PR-3 maps them to IPSO 3313/3334).
 */
class Bmi160 : public I2cSensor {
    public:
        static constexpr std::uint8_t kAddrPrimary   = 0x68;
        static constexpr std::uint8_t kAddrSecondary = 0x69;
        static constexpr std::uint8_t kChipId        = 0xD1;

        /// @brief One IMU sample: raw signed counts, gyro then accel.
        struct Sample {
            std::int16_t gx, gy, gz;   ///< gyroscope X/Y/Z
            std::int16_t ax, ay, az;   ///< accelerometer X/Y/Z
        };

        explicit Bmi160(I2cTransport& bus, std::uint8_t addr = kAddrPrimary)
            : I2cSensor(bus, addr) {}

        /// @brief True if CHIP_ID reads back 0xD1.
        bool probe();
        /// @brief Command accel + gyro into normal power mode. 0 == Ok.
        I2cResult init();
        /// @brief Burst-read the six axis registers. False on bus error.
        bool read(Sample& out);
};

#endif /*__bmi160_hpp__*/
