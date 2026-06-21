#ifndef __lwm2m_object_stubs_hpp__
#define __lwm2m_object_stubs_hpp__

#include <string>

#include "lwm2m_object_3_device.hpp"
#include "lwm2m_object_firmware.hpp"
#include "lwm2m_object_cert.hpp"
#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_object_stubs.hpp
 * @brief Constant-value stub installers for OIDs 4 / 6 / 7.
 *
 * L8 deliverable per RDD REQ-OBJ-005 (Could priority). The three
 * canonical "telemetry" objects are advertised so Discover / Register
 * link-format payloads list them, but their resources return
 * compiled-in placeholders. Upgrading any of these to live readers is
 * a follow-up PR per RDD D6 ("Other 'Could' objects ship as constant
 * stubs … raise them to JSON-backed only when a real deployment asks").
 *
 * The Device object (OID 3) gets its own JSON-backed installer in
 * lwm2m_object_3_device.hpp.
 */

namespace lwm2m { namespace objects {

/// Connectivity Monitoring (OID 4) — RID set per OMA registry.
/// Resources are read-only with placeholder values until a real
/// network-bearer reader lands.
int install_connmon(ObjectStore& store,
                    std::function<std::string()> ipReader = {});

/// Live readers for the Location object (OID 6). Each returns the value as
/// text/plain; an unset hook serves a static "0". Bound to the gps.* ds keys
/// (published by the cellular-client daemon) in the client build.
struct LocationHooks {
    std::function<std::string()> latitude;    ///< /6/0/0
    std::function<std::string()> longitude;   ///< /6/0/1
    std::function<std::string()> altitude;    ///< /6/0/2
    std::function<std::string()> timestamp;   ///< /6/0/5 (epoch seconds)
    std::function<std::string()> speed;       ///< /6/0/6
};

/// Location (OID 6) — Latitude / Longitude / Altitude / Timestamp / Speed.
/// Resources are observable; unset hooks serve static "0".
int install_location(ObjectStore& store, LocationHooks hooks = {});

/// Live readers for the custom Vehicle Telemetry object (OID 33000). Each
/// returns text/plain; an unset hook serves a static default. Bound to the
/// vehicle.* ds keys (published by iot-vehicled) in the client build.
struct VehicleHooks {
    std::function<std::string()> speed;     ///< /33000/0 (km/h)
    std::function<std::string()> rpm;       ///< /33000/1
    std::function<std::string()> coolant;   ///< /33000/2 (deg C)
    std::function<std::string()> throttle;  ///< /33000/3 (%)
    std::function<std::string()> load;      ///< /33000/4 (%)
    std::function<std::string()> fuel;      ///< /33000/5 (%)
    std::function<std::string()> iat;       ///< /33000/6 (deg C)
    std::function<std::string()> maf;       ///< /33000/7 (g/s)
    std::function<std::string()> dtc;       ///< /33000/8 (JSON DTC list)
    std::function<std::string()> link;      ///< /33000/10 (up/down/no-ecu)
};

/// Vehicle Telemetry (OID 33000, private-use range) — OBD-II signals mirrored
/// from vehicle.* ds keys. Observable; unset hooks serve a static default.
/// Custom (not OMA) → installed separately from install_canonical_objects.
int install_vehicle(ObjectStore& store, VehicleHooks hooks = {});

/// Connectivity Statistics (OID 7) — counters.
int install_connstats(ObjectStore& store);

// Security (OID 0) and Server (OID 1) are not seeded from static config —
// they are delivered by the Bootstrap server at /bs and created in the live
// store by the Bootstrap client's apply_commit. (No install_security /
// install_server stubs.)

/// Access Control (OID 2) — single ACE-entry default loaded from
/// apps/config/accessControlObject/0.lua.
int install_access_control(ObjectStore& store, const std::string& configDir);

/// Firmware Update (OID 5) — OTA .ipk via Object 5 (pull). Read-only when
/// no apply hooks are passed; the client supplies ds-backed FwHooks. See
/// lwm2m_object_firmware.hpp.
int install_firmware(ObjectStore& store, const std::string& configDir);

/// Aggregator: installs every canonical OMA Object (0, 1, 2, 3, 4, 5,
/// 6, 7). `fwHooks` wires the Object-5 OTA apply (empty → read-only).
/// Returns the bitwise-OR of installer failures (0 on success).
int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks = {},
                              FwHooks fwHooks = {},
                              CertHooks certHooks = {},
                              LocationHooks locationHooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_stubs_hpp__*/
