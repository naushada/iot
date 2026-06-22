#include <gtest/gtest.h>

#include "lwm2m_registration.hpp"
#include "lwm2m_registration_server.hpp"
#include "coap_adapter.hpp"

#include <arpa/inet.h>

using ::lwm2m::ClientRegistry;
using ::lwm2m::RegistrationServer;
using ::lwm2m::RegistrationOutcome;
using ::lwm2m::ServerRegistration;

namespace {

CoAPAdapter::CoAPMessage make_msg(std::uint8_t method,
                                  const std::vector<std::string>& uriSegments,
                                  const std::vector<std::string>& queries,
                                  const std::string& payload = {}) {
    CoAPAdapter::CoAPMessage m;
    m.coapheader.ver         = 1;
    m.coapheader.type        = 0;       // CON
    m.coapheader.tokenlength = 1;
    m.coapheader.code        = method;  // 2=POST, 4=DELETE
    m.coapheader.msgid       = 0xBEEF;
    m.tokens                 = {0x42};

    for (const auto& seg : uriSegments) {
        CoAPAdapter::CoAPOptions opt;
        opt.optiondelta  = 11;   // Uri-Path absolute number
        opt.optionlength = seg.size();
        opt.optionvalue  = seg;
        m.uripath.push_back(opt);
    }
    for (const auto& q : queries) {
        CoAPAdapter::CoAPOptions opt;
        opt.optiondelta  = 15;   // Uri-Query
        opt.optionlength = q.size();
        opt.optionvalue  = q;
        m.uripath.push_back(opt);
    }
    m.payload = payload;
    return m;
}

/// Parse the 2.01-Created response and pull out the Location-Path option
/// values concatenated as "/rd/{id}".
std::string read_location(const std::string& bytes) {
    if (bytes.size() < 4) return {};
    std::uint8_t tkl = bytes[0] & 0x0F;
    std::size_t i    = 4 + tkl;        // past header + token
    std::uint16_t prev = 0;
    std::string loc;
    while (i < bytes.size() && static_cast<std::uint8_t>(bytes[i]) != 0xFF) {
        std::uint8_t hdr   = bytes[i++];
        std::uint16_t dnib = hdr >> 4;
        std::uint16_t lnib = hdr & 0x0F;

        std::uint16_t delta = dnib;
        if (dnib == 13) { delta = bytes[i++] + 13; }
        else if (dnib == 14) {
            std::uint16_t w = (static_cast<std::uint8_t>(bytes[i]) << 8) |
                              static_cast<std::uint8_t>(bytes[i + 1]);
            i += 2; delta = w + 269;
        }
        std::uint16_t len = lnib;
        if (lnib == 13) { len = bytes[i++] + 13; }
        else if (lnib == 14) {
            std::uint16_t w = (static_cast<std::uint8_t>(bytes[i]) << 8) |
                              static_cast<std::uint8_t>(bytes[i + 1]);
            i += 2; len = w + 269;
        }

        std::uint16_t number = prev + delta;
        prev = number;
        std::string value(bytes.data() + i, len);
        i += len;
        if (number == 8 /*Location-Path*/) {
            loc.push_back('/');
            loc.append(value);
        }
    }
    return loc;
}

std::uint8_t response_code(const std::string& bytes) {
    return bytes.size() >= 2 ? static_cast<std::uint8_t>(bytes[1]) : 0;
}

} // namespace

/* ─────────────────────────── REQ-REG-001 + REQ-REG-002 ───────────────── */

TEST(RegistrationServer, REQ_REG_001_POST_rd_creates_registration) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto msg = make_msg(/*POST*/ 2, {"rd"},
                        {"ep=urn:dev:1", "lt=120", "lwm2m=1.1", "b=U"});

    auto out = srv.handle(msg, coap, "10.0.0.5", 56830);
    EXPECT_EQ(RegistrationOutcome::Created, out.kind);
    EXPECT_FALSE(out.location.empty());
    EXPECT_EQ(1u, registry->size());

    const auto* r = registry->find(out.location);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ("urn:dev:1", r->endpoint);
    EXPECT_EQ(120u, r->lifetime);
    EXPECT_EQ("1.1", r->lwm2mVersion);
    EXPECT_EQ("U",   r->binding);
    EXPECT_EQ("10.0.0.5", r->peerHost);
    EXPECT_EQ(56830u, r->peerPort);
}

TEST(RegistrationServer, REQ_REG_002_response_carries_location_path) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto msg = make_msg(2, {"rd"}, {"ep=urn:dev:1"});
    auto out = srv.handle(msg, coap, "1.2.3.4", 5555);

    EXPECT_EQ(0x41, response_code(out.response));   // 2.01 Created
    EXPECT_EQ(out.location, read_location(out.response));
}

TEST(RegistrationServer, REQ_REG_001_missing_ep_returns_4_00) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto msg = make_msg(2, {"rd"}, {"lt=120"});      // no ep=
    auto out = srv.handle(msg, coap, "1.2.3.4", 5555);
    EXPECT_EQ(RegistrationOutcome::BadRequest, out.kind);
    EXPECT_EQ(0x80, response_code(out.response));    // 4.00 Bad Request
    EXPECT_EQ(0u, registry->size());
}

/* ─────────────────────────── REQ-REG-003 update ──────────────────────── */

TEST(RegistrationServer, REQ_REG_003_POST_rd_loc_updates_lifetime) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto reg = make_msg(2, {"rd"}, {"ep=urn:dev:1", "lt=100"});
    auto created = srv.handle(reg, coap, "1.2.3.4", 5555);
    ASSERT_EQ(RegistrationOutcome::Created, created.kind);

    // Extract the {id} from /rd/{id}.
    std::string id = created.location.substr(4);   // strip "/rd/"

    auto upd = make_msg(2, {"rd", id}, {"lt=600"});
    auto out = srv.handle(upd, coap, "1.2.3.4", 5555);

    EXPECT_EQ(RegistrationOutcome::Updated, out.kind);
    EXPECT_EQ(0x44, response_code(out.response));   // 2.04 Changed
    EXPECT_EQ(600u, registry->find(created.location)->lifetime);
}

TEST(RegistrationServer, reregister_reuses_location_and_refreshes_peer) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto reg1 = make_msg(2, {"rd"}, {"ep=urn:dev:1", "lt=100"});
    auto first = srv.handle(reg1, coap, "1.2.3.4", 5555);
    ASSERT_EQ(RegistrationOutcome::Created, first.kind);

    // Device went offline and re-registered from a new public/ISP address
    // before its lifetime expired: same location, single entry, fresh peer.
    auto reg2 = make_msg(2, {"rd"}, {"ep=urn:dev:1", "lt=100"});
    auto second = srv.handle(reg2, coap, "203.0.113.9", 41000);
    ASSERT_EQ(RegistrationOutcome::Created, second.kind);

    EXPECT_EQ(first.location, second.location);   // /rd/{location} reused
    EXPECT_EQ(1u, registry->size());              // no duplicate registration
    const auto* r = registry->find(second.location);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ("203.0.113.9", r->peerHost);        // ISP IP tracks the latest
}

TEST(RegistrationServer, update_refreshes_peer_address) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto reg = make_msg(2, {"rd"}, {"ep=urn:dev:1", "lt=100"});
    auto created = srv.handle(reg, coap, "1.2.3.4", 5555);
    ASSERT_EQ(RegistrationOutcome::Created, created.kind);
    std::string id = created.location.substr(4);

    // The keepalive Update arrives from a new NAT address → recorded ISP IP
    // follows it (previously frozen at the Register-time address).
    auto upd = make_msg(2, {"rd", id}, {"lt=0"});
    auto out = srv.handle(upd, coap, "198.51.100.7", 33333);
    EXPECT_EQ(RegistrationOutcome::Updated, out.kind);
    EXPECT_EQ("198.51.100.7", registry->find(created.location)->peerHost);
    EXPECT_EQ(33333u, registry->find(created.location)->peerPort);
}

TEST(RegistrationServer, REQ_REG_003_update_unknown_location_4_04) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto upd = make_msg(2, {"rd", "nope"}, {"lt=600"});
    auto out = srv.handle(upd, coap, "1.2.3.4", 5555);
    EXPECT_EQ(RegistrationOutcome::NotFound, out.kind);
    EXPECT_EQ(0x84, response_code(out.response));   // 4.04 Not Found
}

/* ─────────────────────────── REQ-REG-004 deregister ──────────────────── */

TEST(RegistrationServer, REQ_REG_004_DELETE_rd_loc_removes) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto reg = make_msg(2, {"rd"}, {"ep=urn:dev:1"});
    auto created = srv.handle(reg, coap, "1.2.3.4", 5555);
    std::string id = created.location.substr(4);

    auto del = make_msg(/*DELETE*/ 4, {"rd", id}, {});
    auto out = srv.handle(del, coap, "1.2.3.4", 5555);

    EXPECT_EQ(RegistrationOutcome::Removed, out.kind);
    EXPECT_EQ(0x42, response_code(out.response));   // 2.02 Deleted
    EXPECT_EQ(0u, registry->size());
}

/* ─────────────────────────── routing — non-/rd ──────────────────────── */

TEST(RegistrationServer, ignores_non_rd_uri) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    auto msg = make_msg(2, {"bs"}, {"ep=urn:dev:1"});
    auto out = srv.handle(msg, coap, "1.2.3.4", 5555);
    EXPECT_EQ(RegistrationOutcome::None, out.kind);
    EXPECT_TRUE(out.response.empty());
}

/* ─────────────────────────── event callback (D3 hook) ───────────────── */

TEST(RegistrationServer, D3_event_fired_on_create_update_remove) {
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;
    std::vector<RegistrationOutcome::Kind> kinds;
    srv.on_event([&](const RegistrationOutcome& o, const ServerRegistration*) {
        kinds.push_back(o.kind);
    });

    auto reg = make_msg(2, {"rd"}, {"ep=urn:dev:1"});
    srv.handle(reg, coap, "1.2.3.4", 1);
    auto created_loc = registry->all().begin()->first;
    std::string id = created_loc.substr(4);

    auto upd = make_msg(2, {"rd", id}, {"lt=500"});
    srv.handle(upd, coap, "1.2.3.4", 1);

    auto del = make_msg(4, {"rd", id}, {});
    srv.handle(del, coap, "1.2.3.4", 1);

    ASSERT_EQ(3u, kinds.size());
    EXPECT_EQ(RegistrationOutcome::Created, kinds[0]);
    EXPECT_EQ(RegistrationOutcome::Updated, kinds[1]);
    EXPECT_EQ(RegistrationOutcome::Removed, kinds[2]);
}
