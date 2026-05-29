#include "lwm2m_bootstrap_server.hpp"

#include <algorithm>
#include <atomic>
#include <sstream>

#include "lwm2m_coap_builder.hpp"
#include "lwm2m_codec_tlv.hpp"

namespace lwm2m { namespace bootstrap {

using namespace ::lwm2m::coap;

namespace {

/// Server-side message-id allocator. CoAP §4.4 says message IDs MUST be
/// unique within a session window; a monotonic counter is sufficient.
std::uint16_t next_message_id() {
    static std::atomic<std::uint16_t> ctr{1};
    return ctr.fetch_add(1);
}

/* ────────── Security/Server → TLV encoders ────────── */

void append_string_record(std::string& container,
                          std::uint16_t rid, const std::string& value) {
    std::string rec;
    ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11, value, rid, rec);
    container += rec;
}

void append_uint_record(std::string& container,
                        std::uint16_t rid, std::uint32_t value) {
    std::string rec;
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceWithValue_11, value, rid, rec);
    container += rec;
}

void append_bool_record(std::string& container,
                        std::uint16_t rid, bool value) {
    std::string rec;
    ::lwm2m::tlv::encode_bool(TypeBits76_ResourceWithValue_11, value, rid, rec);
    container += rec;
}

std::string encode_security_tlv(const SecurityInstance& s) {
    std::string body;
    append_string_record(body, 0,  s.serverUri);
    append_bool_record(body,   1,  s.isBootstrapServer);
    append_uint_record(body,   2,  s.securityMode);
    if (!s.identity.empty())
        append_string_record(body, 3, s.identity);
    if (!s.secretKey.empty())
        append_string_record(body, 5, s.secretKey);
    append_uint_record(body,   10, s.shortServerId);
    return body;
}

std::string encode_server_tlv(const ServerInstance& s) {
    std::string body;
    append_uint_record(body, 0, s.shortServerId);
    append_uint_record(body, 1, s.lifetime);
    append_string_record(body, 7, s.binding);
    return body;
}

/// Build a PUT /{oid}/{iid} carrying a TLV payload (CF=11542).
std::string build_put(std::uint16_t oid, std::uint16_t iid,
                      const std::string& tlvBody) {
    std::ostringstream ss;
    emit_header(ss, next_message_id(), std::string{},
                METHOD_PUT, TYPE_NON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, std::to_string(oid), prev);
    emit_option(ss, OPT_URI_PATH, std::to_string(iid), prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(11542), prev);
    if (!tlvBody.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << tlvBody;
    }
    return ss.str();
}

/// Build POST /bs (Bootstrap-Finish).
std::string build_bs_finish() {
    std::ostringstream ss;
    emit_header(ss, next_message_id(), std::string{},
                METHOD_POST, TYPE_NON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "bs", prev);
    return ss.str();
}

} // namespace

/* ───────────────────────── Server impl ──────────────────────────────── */

void Server::add_account(AccountProvisioning a) {
    std::string ep = a.endpoint;
    m_accounts[ep] = std::move(a);
}

const AccountProvisioning* Server::find(const std::string& endpoint) const {
    auto it = m_accounts.find(endpoint);
    return it == m_accounts.end() ? nullptr : &it->second;
}

Server::Result Server::handle(const CoAPAdapter::CoAPMessage& msg,
                              CoAPAdapter& coap) {
    Result r;
    const std::string uri = join_uri(msg, coap);
    if (uri != "/bs") return r;     // handled stays false

    const std::uint8_t method = msg.coapheader.code & 0x1F;
    if (method != METHOD_POST) {
        r.handled = true;
        r.frames.push_back(build_ack(msg, /*4.05 Method Not Allowed*/ 0x85));
        return r;
    }

    const std::string ep = get_query(msg, coap, "ep");
    if (ep.empty()) {
        r.handled = true;
        r.frames.push_back(build_ack(msg, /*4.00 Bad Request*/ 0x80));
        return r;
    }
    r.endpoint = ep;

    const auto* acct = find(ep);
    if (!acct) {
        r.handled = true;
        r.frames.push_back(build_ack(msg, /*4.04 Not Found*/ 0x84));
        return r;
    }

    r.handled = true;
    // 1. ACK the /bs request.
    r.frames.push_back(build_ack(msg, /*2.04 Changed*/ 0x44));

    // 2. Push Security Object instances (REQ-BS-007 / REQ-OBJ-001).
    for (const auto& s : acct->security) {
        r.frames.push_back(build_put(/*oid*/ 0, s.iid, encode_security_tlv(s)));
    }
    // 3. Push Server Object instances (REQ-BS-008 short_server_id linkage).
    for (const auto& s : acct->server) {
        r.frames.push_back(build_put(/*oid*/ 1, s.iid, encode_server_tlv(s)));
    }
    // 4. Bootstrap-Finish (REQ-BS-006).
    r.frames.push_back(build_bs_finish());
    return r;
}

}} // namespace lwm2m::bootstrap
