#ifndef __lwm2m_dm_server_hpp__
#define __lwm2m_dm_server_hpp__

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @file lwm2m_dm_server.hpp
 * @brief Device Management & Service Enablement — server-side request builders.
 *
 * L5 deliverable per RDD REQ-DM-009. Pure functions that emit CoAP bytes
 * addressed to a registered client. The caller looks up the peer in
 * `ClientRegistry` and ships the bytes via `UDPAdapter::tx`.
 */

namespace lwm2m { namespace dmsrv {

/// Encodes the four-segment URI shape every builder shares.
/// `iid`, `rid`, `riid` of -1 are omitted.
std::string build_uri(std::uint32_t oid,
                      std::int32_t iid = -1,
                      std::int32_t rid = -1,
                      std::int32_t riid = -1);

/// GET /{path} [Accept: cf]. `acceptCf` of -1 omits the Accept option.
std::string build_read(std::uint16_t messageId,
                       const std::string& token,
                       std::uint32_t oid,
                       std::int32_t iid,
                       std::int32_t rid,
                       std::int32_t acceptCf);

/// GET /{path} + Accept: application/link-format. Convenience wrapper.
std::string build_discover(std::uint16_t messageId,
                           const std::string& token,
                           std::uint32_t oid,
                           std::int32_t iid,
                           std::int32_t rid);

/// PUT /{path} (replace) or POST /{path} (partial). Caller picks the
/// content-format that matches the payload (TLV / plain text / opaque /
/// SenML CBOR…).
std::string build_write(std::uint16_t messageId,
                        const std::string& token,
                        std::uint32_t oid,
                        std::int32_t iid,
                        std::int32_t rid,
                        std::uint16_t cf,
                        const std::string& payload,
                        bool partial);

/// POST /{oid} with a TLV payload describing the new instance.
std::string build_create(std::uint16_t messageId,
                         const std::string& token,
                         std::uint32_t oid,
                         const std::string& tlvPayload);

/// DELETE /{path}.
std::string build_delete(std::uint16_t messageId,
                         const std::string& token,
                         std::uint32_t oid,
                         std::int32_t iid);

/// POST /{path} with optional argument payload.
std::string build_execute(std::uint16_t messageId,
                          const std::string& token,
                          std::uint32_t oid,
                          std::int32_t iid,
                          std::int32_t rid,
                          const std::string& args);

/// Write-Attributes — PUT /{path}?pmin=…&pmax=… with no payload.
/// Each pointer is optional (nullptr means "leave attribute unset").
struct AttributeUpdate {
    const std::uint32_t* pmin{nullptr};
    const std::uint32_t* pmax{nullptr};
    const double*        gt{nullptr};
    const double*        lt{nullptr};
    const double*        st{nullptr};
};

std::string build_write_attributes(std::uint16_t messageId,
                                   const std::string& token,
                                   std::uint32_t oid,
                                   std::int32_t iid,
                                   std::int32_t rid,
                                   const AttributeUpdate& upd);

// ── Custom credential-provisioning object (OID 2048) ────────────────────
// The cloud pushes the VPN client cert family to the device's Object 2048
// (apps/src/lwm2m_object_cert.cpp): one instance per artifact, RID 1 the
// opaque data, RID 3 (instance 0) the Apply execute.
constexpr std::uint32_t kCredObjectOid = 2048;
constexpr std::uint32_t kCredInstCa    = 0;   ///< instance 0, Type="ca"
constexpr std::uint32_t kCredInstCert  = 1;   ///< instance 1, Type="cert"
constexpr std::uint32_t kCredInstKey   = 2;   ///< instance 2, Type="key"
constexpr std::uint32_t kCredRidData   = 1;   ///< RID 1 Certificate Data (opaque, W)
constexpr std::uint32_t kCredRidApply  = 3;   ///< RID 3 Apply (Execute)

/// Build the server→device push sequence for a VPN cert family over Object
/// 2048. Each artifact (CA/cert/key) is zipped + chunked (see
/// lwm2m_cert_chunk.hpp) into one or more opaque WRITEs to /2048/{0,1,2}/1 —
/// large PEMs exceed a single DTLS record, so they cannot be sent whole — then
/// an EXECUTE of Apply /2048/0/3 commits the family. Every frame draws a fresh
/// id from `next_msgid` and carries `token`; ship them to the client's peer in
/// order via UDPAdapter::send_async. Returns the frames in send order.
std::vector<std::string> build_cert_push(const std::function<std::uint16_t()>& next_msgid,
                                         const std::string& token,
                                         const std::string& ca_pem,
                                         const std::string& cert_pem,
                                         const std::string& key_pem);

}} // namespace lwm2m::dmsrv

#endif /*__lwm2m_dm_server_hpp__*/
