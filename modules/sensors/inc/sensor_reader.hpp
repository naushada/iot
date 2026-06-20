#ifndef __sensor_reader_hpp__
#define __sensor_reader_hpp__

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "i2c_bus.hpp"   // I2cTransport / I2cResult
#include "bmi160.hpp"
#include "bme680.hpp"
#include "opt3001.hpp"

/**
 * @file sensor_reader.hpp
 * @brief Latest-value cache + one-shot multi-sensor read for the mangOH Yellow.
 *
 * Pure and ACE-free so it is host-unit-testable against a FakeI2cTransport:
 *
 *   SensorCache  — thread-safe holder of the latest readings; `to_kv()` emits
 *                  the iot.sensor.* batch the iot-sensord daemon publishes.
 *   sample_all   — probe + init + read each chip over an I2cTransport and push
 *                  the results into the cache. No MMIO, no timers — the daemon
 *                  maps the bus and calls this on its tick.
 */

namespace sensors {

/// One published key/value (string-typed, matching the iot.sensor.* schema).
struct KV {
    std::string key;
    std::string value;
};

/// Which chips responded on the last sample.
struct SampleResult {
    bool bme   = false;   ///< BME680 temperature/pressure/humidity
    bool imu   = false;   ///< BMI160 accel/gyro
    bool light = false;   ///< OPT3001 illuminance
    bool any() const { return bme || imu || light; }
};

class SensorCache {
    public:
        void set_env(double temperature_c, double pressure_pa, double humidity_pct);
        void set_lux(double lux);
        void set_imu(std::int16_t ax, std::int16_t ay, std::int16_t az,
                     std::int16_t gx, std::int16_t gy, std::int16_t gz);

        /// The ds publish batch: iot.sensor.* keys + a bumped version. Empty
        /// until at least one chip has been sampled.
        std::vector<KV> to_kv() const;

    private:
        mutable std::mutex m_mtx;
        double  m_tempC = 0, m_pressPa = 0, m_humPct = 0, m_lux = 0;
        std::int16_t m_ax = 0, m_ay = 0, m_az = 0, m_gx = 0, m_gy = 0, m_gz = 0;
        bool m_haveEnv = false, m_haveLux = false, m_haveImu = false;
        std::uint64_t m_version = 0;
};

/// Probe, init and read each sensor over `bus`, updating `cache`. Addresses
/// default to the primary mangOH bus addresses. A chip that fails to probe is
/// skipped (its flag stays false).
SampleResult sample_all(I2cTransport& bus, SensorCache& cache,
                        std::uint8_t bmeAddr   = Bme680::kAddrPrimary,
                        std::uint8_t imuAddr   = Bmi160::kAddrPrimary,
                        std::uint8_t lightAddr = Opt3001::kAddr);

} // namespace sensors

#endif /*__sensor_reader_hpp__*/
