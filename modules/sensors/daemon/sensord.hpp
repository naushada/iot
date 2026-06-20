#ifndef __sensord_hpp__
#define __sensord_hpp__

#include <cstdint>
#include <string>

/**
 * @file sensord.hpp
 * @brief iot-sensord — the privileged mangOH Yellow sensor producer.
 *
 * Owns the I2C bus (maps BSC1 + GPIO via /dev/mem, so it needs root /
 * CAP_SYS_RAWIO — which the unprivileged lwm2m client deliberately lacks),
 * samples the BMI160 / BME680 / OPT3001 on a fixed interval and publishes the
 * iot.sensor.* keys into the data-store. The lwm2m client mirrors those keys
 * into the IPSO objects (PR-3), and the device-ui shows them locally.
 */

namespace sensors {

struct Options {
    std::string   ds_sock;            ///< ds-server socket ("" → built-in default)
    std::string   i2c_dev = "/dev/i2c-1"; ///< kernel i2c-dev node; "" → force BSC /dev/mem
    std::uint32_t interval_sec = 10;  ///< sample cadence
    bool          once = false;       ///< sample once and exit (--once / smoke)
    std::uint8_t  bme_addr   = 0x76;
    std::uint8_t  imu_addr   = 0x68;
    std::uint8_t  light_addr = 0x44;
};

/// Connect to ds, map the bus and run the sample→publish loop. Returns a
/// process exit code (0 ok, non-zero on ds-connect or I2C-map failure so
/// systemd restarts us).
int run(const Options& opt);

} // namespace sensors

#endif /*__sensord_hpp__*/
