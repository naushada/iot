#include "lwm2m_send.hpp"

#include <sstream>

#include "lwm2m_coap_builder.hpp"

namespace lwm2m { namespace send {

using namespace ::lwm2m::coap;

std::string build_send_request(std::uint16_t messageId,
                               const std::string& token,
                               const std::string& payload,
                               int contentFormat) {
    // POST /dp, CON (Send is confirmable so the server ACK lets the client
    // prune the reported samples — ACK-then-prune, §3b of the TDD).
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_POST, TYPE_CON);

    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "dp", prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(contentFormat), prev);

    if (!payload.empty()) {
        ss.put(static_cast<char>(0xFF));   // CoAP payload marker
        ss << payload;
    }
    return ss.str();
}

}} // namespace lwm2m::send
