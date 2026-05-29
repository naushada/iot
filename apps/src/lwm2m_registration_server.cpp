#include "lwm2m_registration_server.hpp"

#include <algorithm>
#include <sstream>

#include "lwm2m_coap_builder.hpp"

namespace lwm2m {

using namespace ::lwm2m::coap;

namespace {

std::uint32_t get_query_u32(const CoAPAdapter::CoAPMessage& msg,
                            CoAPAdapter& helper,
                            const std::string& key,
                            std::uint32_t def) {
    auto v = get_query(msg, helper, key);
    if (v.empty()) return def;
    try {
        return static_cast<std::uint32_t>(std::stoul(v));
    } catch (...) {
        return def;
    }
}

inline std::string build_simple_ack(const CoAPAdapter::CoAPMessage& msg,
                                    std::uint8_t code) {
    return build_ack(msg, code);
}

/// 2.01 Created with `Location-Path: rd` + `Location-Path: {id}` per
/// Core §6.2.2.
std::string build_created_with_location(const CoAPAdapter::CoAPMessage& msg,
                                        const std::string& location) {
    std::ostringstream ss;
    emit_header(ss, msg.coapheader.msgid,
                std::string(msg.tokens.begin(), msg.tokens.end()),
                RSP_201_CREATED, TYPE_ACK);

    // location is "/rd/{id}". Split on '/' and emit Location-Path options.
    std::uint16_t prev = 0;
    std::size_t i = 0;
    while (i < location.size()) {
        if (location[i] != '/') { ++i; continue; }
        ++i;
        std::size_t start = i;
        while (i < location.size() && location[i] != '/') ++i;
        if (i > start) {
            emit_option(ss, OPT_LOCATION_PATH,
                        location.substr(start, i - start), prev);
        }
    }
    return ss.str();
}

} // namespace

/* ───────────────────────── RegistrationServer ─────────────────────── */

RegistrationServer::RegistrationServer(std::shared_ptr<ClientRegistry> reg)
    : m_registry(std::move(reg)) {}

RegistrationOutcome RegistrationServer::handle(const CoAPAdapter::CoAPMessage& msg,
                                               CoAPAdapter& coapHelper,
                                               const std::string& peerHost,
                                               std::uint16_t peerPort) {
    RegistrationOutcome out;

    const std::string uri = join_uri(msg, coapHelper);
    if (uri.empty() || uri.compare(0, 3, "/rd") != 0) {
        return out;  // None — not our request.
    }

    const std::uint8_t method = msg.coapheader.code & 0x1F;
    const bool isRegister     = (uri == "/rd") && (method == 2 /*POST*/);
    const bool isUpdate       = (uri != "/rd") && (uri.compare(0, 4, "/rd/") == 0) &&
                                (method == 2 /*POST*/);
    const bool isDeregister   = (uri != "/rd") && (uri.compare(0, 4, "/rd/") == 0) &&
                                (method == 4 /*DELETE*/);

    if (isRegister) {
        ServerRegistration reg;
        reg.endpoint     = get_query(msg, coapHelper, "ep");
        reg.lifetime     = get_query_u32(msg, coapHelper, "lt", 86400);
        reg.binding      = get_query(msg, coapHelper, "b");
        if (reg.binding.empty()) reg.binding = "U";
        reg.smsNumber    = get_query(msg, coapHelper, "sms");
        reg.lwm2mVersion = get_query(msg, coapHelper, "lwm2m");
        if (reg.lwm2mVersion.empty()) reg.lwm2mVersion = "1.1";
        reg.peerHost     = peerHost;
        reg.peerPort     = peerPort;
        // D2: Short Server ID is part of the bootstrap state, not the
        // Register query. The wrapping ServiceContext_t fills it in via
        // its Security/Server-Object linkage. v1 uses 1 as the default.
        reg.shortServerId = 1;

        if (reg.endpoint.empty()) {
            out.kind     = RegistrationOutcome::BadRequest;
            out.response = build_simple_ack(msg, /*4.00 Bad Request*/ 0x80);
            return out;
        }

        // Decode the link-format payload (may be empty for v1.0-style
        // clients; we accept either way).
        if (!msg.payload.empty()) {
            (void)linkformat::decode(msg.payload, reg.advertisedSet);
        }

        out.location = m_registry->add(std::move(reg));
        out.kind     = RegistrationOutcome::Created;
        out.response = build_created_with_location(msg, out.location);
        if (m_event) m_event(out, m_registry->find(out.location));
        return out;
    }

    if (isUpdate) {
        std::uint32_t newLt = get_query_u32(msg, coapHelper, "lt", 0);
        std::string newBinding = get_query(msg, coapHelper, "b");
        std::vector<linkformat::LinkEntry> newAdvertised;
        std::vector<linkformat::LinkEntry>* advPtr = nullptr;
        if (!msg.payload.empty() &&
            linkformat::decode(msg.payload, newAdvertised) == 0) {
            advPtr = &newAdvertised;
        }

        if (!m_registry->update(uri, newLt, newBinding, advPtr)) {
            out.kind     = RegistrationOutcome::NotFound;
            out.response = build_simple_ack(msg, /*4.04 Not Found*/ 0x84);
            return out;
        }
        out.location = uri;
        out.kind     = RegistrationOutcome::Updated;
        out.response = build_simple_ack(msg, /*2.04 Changed*/ 0x44);
        if (m_event) m_event(out, m_registry->find(uri));
        return out;
    }

    if (isDeregister) {
        if (!m_registry->remove(uri)) {
            out.kind     = RegistrationOutcome::NotFound;
            out.response = build_simple_ack(msg, /*4.04 Not Found*/ 0x84);
            return out;
        }
        out.location = uri;
        out.kind     = RegistrationOutcome::Removed;
        out.response = build_simple_ack(msg, /*2.02 Deleted*/ 0x42);
        if (m_event) m_event(out, nullptr);
        return out;
    }

    // /rd-prefixed URI but unsupported method. 4.05 Method Not Allowed.
    out.kind     = RegistrationOutcome::BadRequest;
    out.response = build_simple_ack(msg, /*4.05*/ 0x85);
    return out;
}

} // namespace lwm2m
