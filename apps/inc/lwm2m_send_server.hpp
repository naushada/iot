#ifndef __lwm2m_send_server_hpp__
#define __lwm2m_send_server_hpp__

#include <string>
#include <vector>

#include "coap_adapter.hpp"
#include "lwm2m_telemetry_pack.hpp"

/**
 * @file lwm2m_send_server.hpp
 * @brief Server-side receiver for the LwM2M 1.1 Send operation (POST /dp).
 *
 * The mirror of lwm2m_send.hpp: where the client builds a /dp POST carrying a
 * SenML pack, this decodes one. Pure + side-effect-free (like
 * RegistrationServer::handle) — it returns the extracted timestamped samples
 * plus a ready-to-ship CoAP ACK; the caller persists the samples (e.g. append
 * to cloud.telemetry.inbox) and transmits the response. Unit-testable without
 * a running server.
 */

namespace lwm2m {

struct SendOutcome {
    enum Kind {
        None,               ///< not a POST /dp — pass to the next handler
        Reported,           ///< a SenML pack was decoded; `samples` populated
        BadRequest,         ///< POST /dp but malformed SenML
        UnsupportedFormat,  ///< POST /dp but Content-Format isn't SenML JSON/CBOR
    };

    Kind kind{None};
    /// Decoded, oldest-first samples (each keeps its real capture time).
    std::vector<telemetry::Sample> samples;
    /// The pack's base path (SenML bn), e.g. "/33000/0/"; combine with each
    /// sample value's relative name to get the full resource path.
    std::string basePath;
    /// CoAP ACK bytes to ship (2.04 on Reported; 4.xx on error; empty on None).
    std::string response;
};

/// Routes POST /dp (LwM2M Send) CoAP messages. Stateless; one instance can
/// serve every peer.
class SendServer {
public:
    SendOutcome handle(const CoAPAdapter::CoAPMessage& msg, CoAPAdapter& coap);
};

} // namespace lwm2m

#endif /*__lwm2m_send_server_hpp__*/
