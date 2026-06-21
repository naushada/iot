#ifndef __lwm2m_send_hpp__
#define __lwm2m_send_hpp__

#include <cstdint>
#include <string>

/**
 * @file lwm2m_send.hpp
 * @brief LwM2M 1.1 "Send" operation — client→server report to /dp.
 *
 * The client reports resource values to the server unsolicited (vs the server
 * Reading them) by POSTing a SenML pack to /dp. This builds that CoAP frame;
 * the SenML pack itself comes from lwm2m::telemetry::build_pack + the SenML
 * codec. Pure (header-only CoAP builder underneath) — msg-id/token allocation
 * and UDP transmission stay with the caller (the registered DM client session).
 *
 * Spec: LwM2M 1.1 §6.4.6 "Send", §8.2.5; transport CoAP POST /dp.
 */

namespace lwm2m { namespace send {

/// LwM2M content-formats for a Send pack (RFC 8428 SenML).
constexpr int CF_SENML_JSON = 110;
constexpr int CF_SENML_CBOR = 112;   // compact default for batched telemetry

/// Build a CoAP CON POST /dp request carrying `payload` (a SenML pack) tagged
/// with `contentFormat`. Returns the wire bytes. `payload` empty → a bodyless
/// POST (no payload marker). The caller owns the msg-id + token (for ACK
/// correlation) and the actual send.
std::string build_send_request(std::uint16_t messageId,
                               const std::string& token,
                               const std::string& payload,
                               int contentFormat = CF_SENML_CBOR);

}} // namespace lwm2m::send

#endif /*__lwm2m_send_hpp__*/
