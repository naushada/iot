#ifndef __bme680_hpp__
#define __bme680_hpp__

#include <cstdint>

#include "i2c_sensor.hpp"

/**
 * @file bme680.hpp
 * @brief Bosch BME680 environmental sensor (mangOH Yellow): pressure,
 *        temperature, humidity (gas/VOC is a follow-up — see note).
 *
 * I²C address 0x76 (SDO low) or 0x77. Flow: probe chip-id (0x61), read the
 * factory calibration coefficients once, then per sample trigger a forced-mode
 * measurement, read the raw T/P/H ADC words and apply Bosch's fixed-point
 * compensation (datasheet §3.3 / the BME68x reference driver). Temperature is
 * computed first because pressure and humidity depend on its `t_fine`.
 *
 * Gas resistance needs heater-profile setup and its own compensation; it is
 * intentionally out of scope for this driver and tracked for IPSO 3325 later.
 */
class Bme680 : public I2cSensor {
    public:
        static constexpr std::uint8_t kAddrPrimary   = 0x76;
        static constexpr std::uint8_t kAddrSecondary = 0x77;
        static constexpr std::uint8_t kChipId        = 0x61;

        /// @brief Factory calibration coefficients (subset for T/P/H).
        struct Calib {
            std::uint16_t t1; std::int16_t t2; std::int8_t  t3;
            std::uint16_t p1; std::int16_t p2; std::int8_t  p3;
            std::int16_t  p4; std::int16_t p5; std::int8_t  p6; std::int8_t p7;
            std::int16_t  p8; std::int16_t p9; std::uint8_t  p10;
            std::uint16_t h1; std::uint16_t h2;
            std::int8_t   h3; std::int8_t  h4; std::int8_t  h5;
            std::uint8_t  h6; std::int8_t  h7;
            std::int32_t  t_fine;   ///< updated by each temperature compensation
        };

        /// @brief One environmental sample in physical units.
        struct Sample {
            double temperature_c;   ///< °C
            double pressure_pa;     ///< Pa
            double humidity_pct;    ///< %RH
        };

        explicit Bme680(I2cTransport& bus, std::uint8_t addr = kAddrPrimary)
            : I2cSensor(bus, addr) {}

        /// @brief True if CHIP_ID reads back 0x61.
        bool probe();
        /// @brief Read calibration + configure oversampling (humidity x1).
        bool init();
        /// @brief Trigger a forced measurement and read compensated T/P/H.
        bool read(Sample& out);

        /// @brief Access the cached calibration (populated by init()).
        const Calib& calib() const { return m_calib; }

        /* ---- Pure compensation primitives (exposed for unit testing) ---- */

        /// @brief Compensated temperature in 0.01 °C; updates `c.t_fine`.
        static std::int32_t compensate_temperature(std::int32_t temp_adc, Calib& c);
        /// @brief Compensated pressure in Pa (call after temperature).
        static std::int32_t compensate_pressure(std::int32_t press_adc, const Calib& c);
        /// @brief Compensated humidity in milli-%RH (call after temperature).
        static std::int32_t compensate_humidity(std::int32_t hum_adc, const Calib& c);

    private:
        bool read_calibration();

        Calib m_calib{};
};

#endif /*__bme680_hpp__*/
