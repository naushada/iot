#ifndef __lwm2m_bootstrap_hpp__
#define __lwm2m_bootstrap_hpp__

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file lwm2m_bootstrap.hpp
 * @brief Shared types for the LwM2M Bootstrap interface (Core §6.1).
 *
 * L4 building block. SecurityInstance and ServerInstance are the
 * in-memory shape both BootstrapClient (staging) and BootstrapServer
 * (provisioning) operate on. They are the high-level mirror of the
 * Security Object (OID 0) and Server Object (OID 1) instances; the
 * codec layer (lwm2m_codec_tlv) translates them to / from the wire.
 *
 * D1 (spec version pinned to 1.1.1) means `lwm2mVersion` defaults to "1.1"
 * and a `lwm2m=1.0` value sent during Bootstrap-Write is rejected.
 */

namespace lwm2m { namespace bootstrap {

/// One Security Object instance.
struct SecurityInstance {
    std::uint16_t iid{0};                 ///< Object Instance ID (0, 1, …)
    std::string   serverUri;              ///< RID 0  — "coaps://host:port"
    bool          isBootstrapServer{false};///< RID 1
    /// RID 2 — 0=PSK, 1=RPK, 2=Certificate, 3=NoSec, 4=Certificate w/EST.
    /// v1 supports 0 (PSK) and 3 (NoSec); other values are rejected at
    /// commit time (REQ-SEC-001 / REQ-SEC-004).
    std::uint8_t  securityMode{3};
    std::string   identity;               ///< RID 3 (binary)
    std::string   serverPublicKey;        ///< RID 4 — opaque (unused for PSK)
    std::string   secretKey;              ///< RID 5 — PSK key bytes (binary)
    std::uint16_t shortServerId{0};       ///< RID 10 — links to ServerInstance.shortServerId
};

/// One Server Object instance.
struct ServerInstance {
    std::uint16_t iid{0};
    std::uint16_t shortServerId{1};       ///< RID 0
    std::uint32_t lifetime{86400};        ///< RID 1
    std::string   binding{"U"};           ///< RID 7 — D1 1.1 allows U/T/UQ/TQ
};

/// One provisioning record for a single LwM2M Client endpoint.
struct AccountProvisioning {
    std::string                  endpoint;        ///< matches ep=… on Register
    std::vector<SecurityInstance> security;       ///< Security Object instances
    std::vector<ServerInstance>   server;         ///< Server Object instances
};

}} // namespace lwm2m::bootstrap

#endif /*__lwm2m_bootstrap_hpp__*/
