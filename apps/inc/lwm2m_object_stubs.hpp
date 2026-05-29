#ifndef __lwm2m_object_stubs_hpp__
#define __lwm2m_object_stubs_hpp__

#include <string>

#include "lwm2m_object_3_device.hpp"
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
int install_connmon(ObjectStore& store);

/// Location (OID 6) — Latitude / Longitude / Timestamp.
int install_location(ObjectStore& store);

/// Connectivity Statistics (OID 7) — counters.
int install_connstats(ObjectStore& store);

/// Aggregator: installs Device + Connectivity Monitoring + Location +
/// Connectivity Statistics. Returns the bitwise-OR of installer
/// failures (0 on success).
int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_stubs_hpp__*/
