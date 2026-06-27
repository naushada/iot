#include "lwm2m_registration_client.hpp"

#include <algorithm>
#include <sstream>

#include "lwm2m_coap_builder.hpp"

namespace lwm2m {

using namespace ::lwm2m::coap;

/* ───────────────────────── RegistrationClient ────────────────────── */

RegistrationClient::RegistrationClient(ClientConfig cfg,
                                       const ObjectStore& store)
    : m_cfg(std::move(cfg)),
      m_lifetime(m_cfg.lifetime),
      m_endpoint(m_cfg.endpoint),
      m_store(store) {}

std::string RegistrationClient::build_keepalive_ping(std::uint16_t messageId) {
    // RFC 7252 §4.3: an empty Confirmable (no token, code 0.00) is a CoAP ping.
    std::ostringstream ss;
    emit_header(ss, messageId, /*token*/"", /*code 0.00*/0x00, TYPE_CON);
    return ss.str();
}

void RegistrationClient::set_endpoint(std::string ep) {
    {
        std::lock_guard<std::mutex> g(m_endpoint_mtx);
        if (m_endpoint == ep) return;   // unchanged — don't trigger re-register
        m_endpoint = std::move(ep);
    }
    m_re_register_pending.store(true, std::memory_order_relaxed);
}

std::string RegistrationClient::endpoint() const {
    std::lock_guard<std::mutex> g(m_endpoint_mtx);
    return m_endpoint;
}

std::string RegistrationClient::build_register_request(std::uint16_t messageId,
                                                       const std::string& token) {
    // REQ-REG-001: POST /rd?ep=…&lt=…&lwm2m=1.1&b=…[&sms=…]
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_POST, TYPE_CON);

    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "rd", prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(40), prev);

    // Query options must be emitted in option-number order: 15 > 12 > 11.
    emit_option(ss, OPT_URI_QUERY, "ep=" + endpoint(), prev);
    emit_option(ss, OPT_URI_QUERY,
                "lt=" + std::to_string(lifetime()), prev);
    emit_option(ss, OPT_URI_QUERY,
                "lwm2m=" + m_cfg.lwm2mVersion, prev);
    emit_option(ss, OPT_URI_QUERY, "b=" + m_cfg.binding, prev);
    if (!m_cfg.smsNumber.empty()) {
        emit_option(ss, OPT_URI_QUERY, "sms=" + m_cfg.smsNumber, prev);
    }

    // Payload marker + link-format from the ObjectStore.
    auto entries = linkformat::register_payload(m_store);
    std::string payload = linkformat::encode(entries);
    if (!payload.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << payload;
    }

    m_lastSendOrAck = std::chrono::steady_clock::now();
    m_state         = RegistrationState::AwaitingRegisterAck;
    return ss.str();
}

std::string RegistrationClient::build_update_request(std::uint16_t messageId,
                                                     const std::string& token,
                                                     bool withAdvertisedSet) {
    // REQ-REG-003: POST /rd/{loc}[?lt=…&b=…] [link-format payload].
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_POST, TYPE_CON);

    std::uint16_t prev = 0;
    emit_uri_path(ss, m_location, prev);

    emit_option(ss, OPT_URI_QUERY,
                "lt=" + std::to_string(lifetime()), prev);
    emit_option(ss, OPT_URI_QUERY, "b=" + m_cfg.binding, prev);

    if (withAdvertisedSet) {
        emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(40), prev);
        auto entries = linkformat::register_payload(m_store);
        std::string payload = linkformat::encode(entries);
        if (!payload.empty()) {
            ss.put(static_cast<char>(0xFF));
            ss << payload;
        }
    }

    m_lastSendOrAck = std::chrono::steady_clock::now();
    m_state         = RegistrationState::AwaitingUpdateAck;
    return ss.str();
}

std::string RegistrationClient::build_deregister_request(std::uint16_t messageId,
                                                         const std::string& token) {
    // REQ-REG-004: DELETE /rd/{loc}.
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_DELETE, TYPE_CON);

    std::uint16_t prev = 0;
    emit_uri_path(ss, m_location, prev);

    // Stamp the send so check_ack_timeout() measures the Deregister ack from
    // here (register/update stamp it too); otherwise a stale timestamp would
    // make the deadline fire on the next tick.
    m_lastSendOrAck = std::chrono::steady_clock::now();
    m_state = RegistrationState::AwaitingDeregisterAck;
    return ss.str();
}

void RegistrationClient::on_response(const CoAPAdapter::CoAPMessage& msg,
                                     CoAPAdapter& coapHelper) {
    const std::uint8_t code = msg.coapheader.code;
    const std::uint8_t cls  = code >> 5;
    const bool        ok    = (cls == 2);

    auto extractLocation = [&]() {
        std::string out;
        for (const auto& opt : msg.uripath) {
            if (coapHelper.getOptionNumber(opt.optiondelta) == "Location-Path") {
                out.push_back('/');
                out.append(opt.optionvalue);
            }
        }
        return out;
    };

    switch (m_state) {
        case RegistrationState::AwaitingRegisterAck:
            m_ackRetransmits = 0;            // a response arrived — reset budget
            if (ok && code == 0x41 /*2.01*/) {
                m_location      = extractLocation();
                m_state         = RegistrationState::Registered;
                m_lastSendOrAck = std::chrono::steady_clock::now();
            } else {
                m_state = RegistrationState::Failed;
            }
            return;

        case RegistrationState::AwaitingUpdateAck:
            m_ackRetransmits = 0;            // a response arrived — reset budget
            if (ok) {
                m_state         = RegistrationState::Registered;
                m_lastSendOrAck = std::chrono::steady_clock::now();
            } else {
                m_state = RegistrationState::Failed;
            }
            return;

        case RegistrationState::AwaitingDeregisterAck:
            m_ackRetransmits = 0;            // a response arrived — reset budget
            if (ok) {
                m_state    = RegistrationState::Unregistered;
                m_location.clear();
            } else {
                m_state = RegistrationState::Failed;
            }
            return;

        default:
            return;
    }
}

bool RegistrationClient::should_send_update(
    std::chrono::steady_clock::time_point now) const {
    if (m_state != RegistrationState::Registered) return false;
    auto due = m_lastSendOrAck
             + std::chrono::seconds(lifetime())
             - std::chrono::seconds(m_cfg.updateMarginSeconds);
    return now >= due;
}

RegistrationClient::AckRecovery RegistrationClient::check_ack_timeout(
    std::chrono::steady_clock::time_point now) {
    const bool awaiting =
        m_state == RegistrationState::AwaitingRegisterAck   ||
        m_state == RegistrationState::AwaitingUpdateAck     ||
        m_state == RegistrationState::AwaitingDeregisterAck;
    if (!awaiting) return AckRecovery::None;

    const auto deadline =
        m_lastSendOrAck + std::chrono::seconds(m_cfg.ackTimeoutSeconds);
    if (now < deadline) return AckRecovery::None;        // still within window

    // A lost Update ack with retransmits left: the DTLS session most likely
    // just dropped a datagram, so a fresh Update is the cheap fix. Revert to
    // Registered so the caller's build_update_request() re-arms the FSM (sets
    // AwaitingUpdateAck + m_lastSendOrAck = now).
    if (m_state == RegistrationState::AwaitingUpdateAck &&
        m_ackRetransmits < m_cfg.maxAckRetransmits) {
        ++m_ackRetransmits;
        m_state = RegistrationState::Registered;
        return AckRecovery::RetransmitUpdate;
    }

    // Budget spent, or a Register/Deregister ack never came: the path to the
    // server has been down long enough that the session is suspect. Drop to
    // Unregistered and tell the caller to reconnect + re-Register.
    m_ackRetransmits = 0;
    m_state    = RegistrationState::Unregistered;
    m_location.clear();
    return AckRecovery::ReRegister;
}

void RegistrationClient::note_update_sent(
    std::chrono::steady_clock::time_point t) {
    m_lastSendOrAck = t;
}

} // namespace lwm2m
