#ifndef __lwm2m_object_sensors_hpp__
#define __lwm2m_object_sensors_hpp__

#include <functional>
#include <string>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_object_sensors.hpp
 * @brief IPSO sensor objects for the mangOH Yellow sensor block (PR-3).
 *
 * `install_sensors` registers the standard IPSO smart-object descriptors for
 * the onboard chips so the cloud/DM gets them through the existing Read /
 * Observe machinery with no new transport:
 *
 *   3301 Illuminance   (light)   5700 value, 5701 units
 *   3303 Temperature   (BME680)  5700 value, 5701 units
 *   3304 Humidity      (BME680)  5700 value, 5701 units
 *   3315 Barometer     (BME680)  5700 value, 5701 units
 *   3313 Accelerometer (BMI160)  5702/5703/5704 X/Y/Z, 5701 units
 *   3334 Gyrometer     (BMI160)  5702/5703/5704 X/Y/Z, 5701 units
 *
 * Each value resource's read closure comes from `SensorHooks` (bound to the
 * sampler's cache in production); an unset hook yields a static "0" so the
 * object still advertises on a board without the mangOH attached. The objects
 * are pure model state — no hardware is touched here — so this installer is
 * host-unit-testable exactly like install_device.
 */

namespace lwm2m { namespace objects {

/// Per-channel value readers. Each returns the latest reading as text/plain.
/// Leave a field null to serve a static "0" for that channel.
struct SensorHooks {
    std::function<std::string()> temperature;   ///< 3303/0/5700 (Cel)
    std::function<std::string()> humidity;      ///< 3304/0/5700 (%RH)
    std::function<std::string()> pressure;      ///< 3315/0/5700 (Pa)
    std::function<std::string()> illuminance;   ///< 3301/0/5700 (lx)
    std::function<std::string()> accel_x;       ///< 3313/0/5702 (m/s2)
    std::function<std::string()> accel_y;       ///< 3313/0/5703
    std::function<std::string()> accel_z;       ///< 3313/0/5704
    std::function<std::string()> gyro_x;        ///< 3334/0/5702 (deg/s)
    std::function<std::string()> gyro_y;        ///< 3334/0/5703
    std::function<std::string()> gyro_z;        ///< 3334/0/5704
};

/// Install the IPSO sensor objects into `store`. Returns 0 (always succeeds;
/// add_object is infallible).
int install_sensors(ObjectStore& store, SensorHooks hooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_sensors_hpp__*/
