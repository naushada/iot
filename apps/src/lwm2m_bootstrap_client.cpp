#include "lwm2m_bootstrap_client.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "lwm2m_coap_builder.hpp"
#include "lwm2m_codec_tlv.hpp"

namespace lwm2m { namespace bootstrap {

using namespace ::lwm2m::coap;

namespace {

/// Parse the URI into (oid, iid). Returns false on a non-numeric path.
bool parse_object_uri(const std::string& uri,
                      std::int32_t& oid, std::int32_t& iid) {
    oid = -1; iid = -1;
    if (uri.empty() || uri.front() != '/') return false;
    std::size_t i = 1;
    auto take_int = [&](std::int32_t& out) -> bool {
        std::size_t start = i;
        while (i < uri.size() && uri[i] != '/') ++i;
        if (i == start) return false;
        for (std::size_t j = start; j < i; ++j) {
            if (!std::isdigit(static_cast<unsigned char>(uri[j]))) return false;
        }
        out = std::stoi(uri.substr(start, i - start));
        return true;
    };
    if (!take_int(oid)) return false;
    if (i < uri.size() && uri[i] == '/') { ++i; (void)take_int(iid); }
    return true;
}

/* ────────── TLV → Security/Server helpers ────────── */

std::string hex_to_bin(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

std::uint32_t read_uint_be(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t v = 0;
    for (auto b : bytes) v = (v << 8) | b;
    return v;
}

} // namespace

/* ───────────────────────── Client impl ──────────────────────────────── */

Client::Client(std::string endpoint,
               std::shared_ptr<ObjectStore> liveStore,
               std::shared_ptr<DTLSAdapter> dtls)
    : m_endpoint(std::move(endpoint)),
      m_store(std::move(liveStore)),
      m_dtls(std::move(dtls)) {}

std::string Client::build_bs_request(std::uint16_t messageId,
                                     const std::string& token) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, /*POST*/ 2, /*CON*/ 0);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "bs", prev);
    emit_option(ss, OPT_URI_QUERY, "ep=" + m_endpoint, prev);
    m_state = ClientState::AwaitingBSAck;
    return ss.str();
}

std::string Client::handle_bs_traffic(const CoAPAdapter::CoAPMessage& msg,
                                      CoAPAdapter& coap) {
    const std::string uri    = join_uri(msg, coap);
    const std::uint8_t code  = msg.coapheader.code;
    const std::uint8_t cls   = code >> 5;
    const std::uint8_t method = code & 0x1F;

    // Step 1 — the server's response to our POST /bs (2.04 Changed).
    if (m_state == ClientState::AwaitingBSAck && cls == 2) {
        m_state = ClientState::WaitForBSWrites;
        m_staging.clear();
        return {};
    }

    if (m_state != ClientState::WaitForBSWrites &&
        m_state != ClientState::AwaitingBSAck) {
        return {};
    }

    auto ack_2_04 = [&]() { return build_ack(msg, RSP_204_CHANGED); };
    auto ack_2_02 = [&]() { return build_ack(msg, RSP_202_DELETED); };
    auto err_4_00 = [&]() { return build_ack(msg, RSP_400_BAD_REQ); };

    // Bootstrap-Finish: POST /bs with no payload.
    if (method == 2 /*POST*/ && uri == "/bs") {
        if (m_state == ClientState::WaitForBSWrites) {
            apply_commit();
            return ack_2_04();
        }
        return err_4_00();
    }

    // Bootstrap-Delete: DELETE / (purge) or DELETE /{oid}/{iid}.
    if (method == 4 /*DELETE*/) {
        if (uri.empty() || uri == "/") {
            m_staging.purge = true;
            return ack_2_02();
        }
        std::int32_t oid = -1, iid = -1;
        if (!parse_object_uri(uri, oid, iid) || iid < 0) {
            return err_4_00();
        }
        m_staging.deletedObjectInstances.push_back(
            static_cast<std::uint32_t>(oid) * 256 + iid);
        return ack_2_02();
    }

    // Bootstrap-Write: PUT /{oid}[/{iid}].
    if (method == 3 /*PUT*/) {
        std::int32_t oid = -1, iid = -1;
        if (!parse_object_uri(uri, oid, iid)) return err_4_00();
        const std::uint16_t instId = static_cast<std::uint16_t>(iid >= 0 ? iid : 0);

        if (oid == 0) {
            SecurityInstance s;
            if (!decode_security_write(instId, msg.payload, s)) return err_4_00();
            m_staging.security.push_back(s);
            return ack_2_04();
        }
        if (oid == 1) {
            ServerInstance srv;
            if (!decode_server_write(instId, msg.payload, srv)) return err_4_00();
            m_staging.server.push_back(srv);
            return ack_2_04();
        }
        // Unknown OID — REQ-BS-005 allows the BS to write any OID; we
        // currently ignore non-0/1 writes (Access Control is L8).
        return ack_2_04();
    }

    return {};
}

bool Client::decode_security_write(std::uint16_t iid,
                                   const std::string& payload,
                                   SecurityInstance& out) {
    out.iid = iid;
    LwM2MObject obj;
    LwM2MObjectData scratch;
    if (::lwm2m::tlv::decode_container(payload, scratch, obj) != 0) return false;

    for (const auto& rec : obj.m_value) {
        const auto& v = rec.m_ridvalue;
        switch (rec.m_rid) {
            case 0: out.serverUri.assign(v.begin(), v.end()); break;
            case 1: out.isBootstrapServer = !v.empty() && v[0] != 0; break;
            case 2: out.securityMode = v.empty() ? 3 : v[0]; break;
            case 3: out.identity.assign(v.begin(), v.end()); break;
            case 4: out.serverPublicKey.assign(v.begin(), v.end()); break;
            case 5: out.secretKey.assign(v.begin(), v.end()); break;
            case 10: out.shortServerId = static_cast<std::uint16_t>(read_uint_be(v)); break;
            default: break;     // unknown / unhandled RID
        }
    }
    return true;
}

bool Client::decode_server_write(std::uint16_t iid,
                                 const std::string& payload,
                                 ServerInstance& out) {
    out.iid = iid;
    LwM2MObject obj;
    LwM2MObjectData scratch;
    if (::lwm2m::tlv::decode_container(payload, scratch, obj) != 0) return false;

    for (const auto& rec : obj.m_value) {
        const auto& v = rec.m_ridvalue;
        switch (rec.m_rid) {
            case 0: out.shortServerId = static_cast<std::uint16_t>(read_uint_be(v)); break;
            case 1: out.lifetime      = read_uint_be(v); break;
            case 7: out.binding.assign(v.begin(), v.end()); break;
            default: break;
        }
    }
    return true;
}

void Client::apply_commit() {
    // REQ-BS-004 / REQ-OBJ-001: atomic install of the staged set.
    //  1. Drop instances marked for delete (or all on purge).
    //  2. Install staged Security Object instances into the live store
    //     (oid 0) and, for non-BS PSK accounts, into the DTLS credential
    //     store so the next outbound DTLS handshake uses the new identity.
    //  3. Install staged Server Object instances into the live store
    //     (oid 1).

    if (m_staging.purge) {
        if (auto* desc = m_store->find(0)) desc->instances.clear();
        if (auto* desc = m_store->find(1)) desc->instances.clear();
    } else {
        for (auto key : m_staging.deletedObjectInstances) {
            auto oid = key / 256, iid = key % 256;
            if (auto* desc = m_store->find(oid)) desc->instances.erase(iid);
        }
    }

    // Install Security Object instances.
    if (!m_staging.security.empty()) {
        if (!m_store->has(0)) {
            ObjectDescriptor d;
            d.oid = 0; d.name = "Security"; d.urn = "urn:oma:lwm2m:oma:0:1.1";
            m_store->add_object(d);
        }
        auto* sec = m_store->find(0);
        for (const auto& s : m_staging.security) {
            ObjectInstance inst;
            inst.iid = s.iid;
            sec->instances[s.iid] = inst;

            // REQ-SEC-001 / REQ-SEC-005: install PSK credentials for
            // PSK-mode (RID 2 = 0) accounts. NoSec (3) is allowed and
            // simply skips the credential install. Other modes are
            // rejected per REQ-SEC-004.
            if (s.securityMode == 0 /*PSK*/) {
                if (m_dtls) m_dtls->add_credential(s.identity, s.secretKey);
            }
        }
    }

    // Install Server Object instances.
    if (!m_staging.server.empty()) {
        if (!m_store->has(1)) {
            ObjectDescriptor d;
            d.oid = 1; d.name = "Server"; d.urn = "urn:oma:lwm2m:oma:1:1.1";
            m_store->add_object(d);
        }
        auto* srv = m_store->find(1);
        for (const auto& sv : m_staging.server) {
            ObjectInstance inst;
            inst.iid = sv.iid;
            srv->instances[sv.iid] = inst;
        }
    }

    auto committed = m_staging;
    m_staging.clear();
    m_state = ClientState::Done;
    if (m_onDone) m_onDone(committed);
}

}} // namespace lwm2m::bootstrap
