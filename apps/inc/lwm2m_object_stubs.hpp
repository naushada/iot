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

/// Security (OID 0) — read-only mirror of every
/// apps/config/securityObject/<iid>.lua under configDir; one
/// ObjectInstance per matching file. Lets `read path=/0/<iid>/<rid>`
/// and the CLI alias `security read=<rid>` return real data. The
/// Bootstrap-server delivery path still uses the AccountProvisioning
/// loader in main.cpp.
int install_security(ObjectStore& store, const std::string& configDir);

/// Server (OID 1) — read-only mirror of
/// apps/config/serverObject/0.lua (and /1.lua when populated).
int install_server(ObjectStore& store, const std::string& configDir);

/// Access Control (OID 2) — single ACE-entry default loaded from
/// apps/config/accessControlObject/0.lua.
int install_access_control(ObjectStore& store, const std::string& configDir);

/// Firmware Update (OID 5) — read-only state/version stubs from
/// apps/config/firmwareObject/0.lua. Package-pull / apply hooks are
/// a follow-up.
int install_firmware(ObjectStore& store, const std::string& configDir);

/// Aggregator: installs every canonical OMA Object (0, 1, 2, 3, 4, 5,
/// 6, 7). Returns the bitwise-OR of installer failures (0 on success).
int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_stubs_hpp__*/
