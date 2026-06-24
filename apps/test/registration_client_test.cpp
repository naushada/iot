#include <gtest/gtest.h>

#include "lwm2m_registration_client.hpp"
#include "lwm2m_registration_server.hpp"
#include "lwm2m_registration.hpp"
#include "lwm2m_object_store.hpp"
#include "coap_adapter.hpp"

using ::lwm2m::ClientConfig;
using ::lwm2m::ClientRegistry;
using ::lwm2m::ObjectDescriptor;
using ::lwm2m::ObjectInstance;
using ::lwm2m::ObjectStore;
using ::lwm2m::RegistrationClient;
using ::lwm2m::RegistrationServer;
using ::lwm2m::RegistrationState;
using ::lwm2m::ResourceType;
using ::lwm2m::Operations;

namespace {

ObjectStore minimal_store() {
    ObjectStore s;
    ObjectDescriptor d1;
    d1.oid = 1; d1.name = "Server"; d1.urn = "urn:oma:lwm2m:oma:1:1.1";
    ObjectInstance i1; i1.iid = 0;
    d1.instances[0] = i1;
    s.add_object(d1);

    ObjectDescriptor d3;
    d3.oid = 3; d3.name = "Device"; d3.urn = "urn:oma:lwm2m:oma:3:1.1";
    ObjectInstance i3; i3.iid = 0;
    d3.instances[0] = i3;
    s.add_object(d3);
    return s;
}

ClientConfig minimal_cfg(std::uint32_t lt = 3600) {
    ClientConfig c;
    c.endpoint     = "urn:dev:client-1";
    c.lifetime     = lt;
    c.binding      = "U";
    c.lwm2mVersion = "1.1";
    return c;
}

// Keeps a server + registry alive so a client can be driven Registered and
// then fed real Update acks (build_*_request → server → on_response).
struct Harness {
    std::shared_ptr<ClientRegistry> registry = std::make_shared<ClientRegistry>();
    RegistrationServer              srv{registry};
    CoAPAdapter                     coap;

    void feed_register_ack(RegistrationClient& cli) {
        auto wire = cli.build_register_request(1, std::string{0x01});
        CoAPAdapter::CoAPMessage parsed; coap.parseRequest(wire, parsed);
        auto outcome = srv.handle(parsed, coap, "1.1.1.1", 1);
        CoAPAdapter::CoAPMessage ack;    coap.parseRequest(outcome.response, ack);
        cli.on_response(ack, coap);
    }
    void feed_update_ack(RegistrationClient& cli) {
        auto wire = cli.build_update_request(3, std::string{0x03}, false);
        CoAPAdapter::CoAPMessage parsed; coap.parseRequest(wire, parsed);
        auto outcome = srv.handle(parsed, coap, "1.1.1.1", 1);
        CoAPAdapter::CoAPMessage ack;    coap.parseRequest(outcome.response, ack);
        cli.on_response(ack, coap);
    }
};

} // namespace

/* ─────────────────────────── REQ-REG-001 / REQ-REG-007 ───────────────── */

TEST(RegistrationClient, REQ_REG_001_build_register_request_is_routed_to_rd) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(), store);
    std::string token{0x77};
    auto bytes = cli.build_register_request(0x1234, token);

    // Round-trip through CoAPAdapter::parseRequest to make sure the
    // emitted wire format is a parseable POST /rd with the right
    // queries and a link-format payload.
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(bytes, parsed);

    EXPECT_EQ(0x1234, parsed.coapheader.msgid);
    EXPECT_EQ(2u, parsed.coapheader.code);          // POST

    // Walk options for Uri-Path and Uri-Query.
    std::vector<std::string> path, query;
    for (const auto& opt : parsed.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Path")  path.push_back(opt.optionvalue);
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Query") query.push_back(opt.optionvalue);
    }
    ASSERT_EQ(1u, path.size());
    EXPECT_EQ("rd", path[0]);

    auto find_q = [&](const std::string& prefix) -> std::string {
        for (const auto& q : query) {
            if (q.compare(0, prefix.size(), prefix) == 0) return q.substr(prefix.size());
        }
        return {};
    };
    EXPECT_EQ("urn:dev:client-1", find_q("ep="));
    EXPECT_EQ("3600",             find_q("lt="));
    EXPECT_EQ("1.1",              find_q("lwm2m="));
    EXPECT_EQ("U",                find_q("b="));

    // Payload contains the link-format with the root entry.
    EXPECT_NE(std::string::npos,
              parsed.payload.find("rt=\"oma.lwm2m\""));
    EXPECT_NE(std::string::npos, parsed.payload.find("/1/0"));
    EXPECT_NE(std::string::npos, parsed.payload.find("/3/0"));

    EXPECT_EQ(RegistrationState::AwaitingRegisterAck, cli.state());
}

/* ─────────────────────────── REQ-REG-002 ack consumption ─────────────── */

TEST(RegistrationClient, REQ_REG_002_consumes_2_01_with_location_path) {
    auto store = minimal_store();
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    RegistrationClient cli(minimal_cfg(), store);
    auto wire = cli.build_register_request(0x4321, std::string{0x99});

    // Feed through the real RegistrationServer to get a real 2.01 reply.
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    auto outcome = srv.handle(parsed, coap, "10.0.0.5", 56830);
    ASSERT_FALSE(outcome.response.empty());

    CoAPAdapter::CoAPMessage rsp;
    coap.parseRequest(outcome.response, rsp);
    cli.on_response(rsp, coap);

    EXPECT_EQ(RegistrationState::Registered, cli.state());
    EXPECT_EQ(outcome.location, cli.location());
}

/* ─────────────────────────── FUP-2 dispatch wiring ───────────────────── */

TEST(RegistrationClient, FUP_2_processRequest_dispatches_ack_to_on_response) {
    // The L9 ACK short-circuit in CoAPAdapter::processRequest must
    // forward Acknowledgement-typed frames to the attached
    // RegistrationClient so its FSM advances. Without this dispatch the
    // client would stay in AwaitingRegisterAck forever and the Update
    // tick would never fire (see log/L9/results.md FUP-2).
    auto store = minimal_store();
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;
    auto cli = std::make_shared<RegistrationClient>(minimal_cfg(), store);

    auto wire = cli->build_register_request(0x4321, std::string{0x99});
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    auto outcome = srv.handle(parsed, coap, "10.0.0.5", 56830);
    ASSERT_FALSE(outcome.response.empty());

    // FUP-2: attach the client to the adapter, then route the 2.01 reply
    // through processRequest (which is what the live wire path does).
    coap.registrationClient(cli);
    std::vector<std::string> out;
    coap.processRequest(/*isAmIClient*/ true, outcome.response, out);

    // ACK short-circuit must produce no outbound bytes…
    EXPECT_TRUE(out.empty());
    // …and the FSM must have advanced.
    EXPECT_EQ(RegistrationState::Registered, cli->state());
    EXPECT_EQ(outcome.location, cli->location());
}

/* ─────────────────────────── REQ-REG-003 / REQ-REG-006 ───────────────── */

TEST(RegistrationClient, REQ_REG_006_should_send_update_at_margin) {
    auto store = minimal_store();
    auto cfg   = minimal_cfg(/*lt*/ 100);
    cfg.updateMarginSeconds = 30;
    RegistrationClient cli(cfg, store);

    // Force into Registered state via the public ack consumption path.
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;
    auto wire    = cli.build_register_request(1, std::string{0x01});
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    auto outcome = srv.handle(parsed, coap, "1.1.1.1", 1);
    CoAPAdapter::CoAPMessage ack;
    coap.parseRequest(outcome.response, ack);
    cli.on_response(ack, coap);
    ASSERT_EQ(RegistrationState::Registered, cli.state());

    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cli.should_send_update(t0));
    // Just after the margin: due.
    EXPECT_TRUE(cli.should_send_update(t0 + std::chrono::seconds(71)));
}

TEST(RegistrationClient, REQ_REG_003_build_update_emits_location_path) {
    // Drive the client to Registered, then build an Update.
    auto store = minimal_store();
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    RegistrationClient cli(minimal_cfg(), store);
    auto reg_wire = cli.build_register_request(0xAAAA, std::string{0x01});
    CoAPAdapter::CoAPMessage reg_parsed;
    coap.parseRequest(reg_wire, reg_parsed);
    auto outcome = srv.handle(reg_parsed, coap, "1.1.1.1", 1);
    CoAPAdapter::CoAPMessage ack;
    coap.parseRequest(outcome.response, ack);
    cli.on_response(ack, coap);
    ASSERT_EQ(RegistrationState::Registered, cli.state());

    auto upd_wire = cli.build_update_request(0xBBBB, std::string{0x02},
                                             /*withAdvertisedSet*/ false);
    CoAPAdapter::CoAPMessage upd;
    coap.parseRequest(upd_wire, upd);

    std::vector<std::string> path;
    for (const auto& opt : upd.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Path") {
            path.push_back(opt.optionvalue);
        }
    }
    ASSERT_GE(path.size(), 2u);
    EXPECT_EQ("rd",   path[0]);
    EXPECT_EQ(cli.location().substr(4), path[1]);   // "/rd/X" → "X"
}

/* ─────────────────────────── REQ-REG-004 deregister ──────────────────── */

TEST(RegistrationClient, REQ_REG_004_build_deregister_is_DELETE_rd_loc) {
    auto store = minimal_store();
    auto registry = std::make_shared<ClientRegistry>();
    RegistrationServer srv(registry);
    CoAPAdapter coap;

    RegistrationClient cli(minimal_cfg(), store);
    auto reg_wire = cli.build_register_request(1, std::string{0x01});
    CoAPAdapter::CoAPMessage reg_parsed;
    coap.parseRequest(reg_wire, reg_parsed);
    auto outcome = srv.handle(reg_parsed, coap, "1.1.1.1", 1);
    CoAPAdapter::CoAPMessage ack;
    coap.parseRequest(outcome.response, ack);
    cli.on_response(ack, coap);

    auto wire = cli.build_deregister_request(2, std::string{0x02});
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    EXPECT_EQ(4u, parsed.coapheader.code);    // DELETE

    auto del_outcome = srv.handle(parsed, coap, "1.1.1.1", 1);
    EXPECT_EQ(::lwm2m::RegistrationOutcome::Removed, del_outcome.kind);
    EXPECT_EQ(0u, registry->size());
}

/* ─────────────────────────── FUP-DS-9 / FUP-DS-10 live setters ───────── */

TEST(RegistrationClient, FUP_DS_9_set_lifetime_takes_effect_on_next_build) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(/*lt=*/3600), store);

    cli.set_lifetime(900);
    EXPECT_EQ(900u, cli.lifetime());

    auto bytes = cli.build_register_request(0x1234, std::string{0x77});
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(bytes, parsed);

    std::string lt;
    for (const auto& opt : parsed.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Query" &&
            opt.optionvalue.compare(0, 3, "lt=") == 0) {
            lt = opt.optionvalue.substr(3);
        }
    }
    EXPECT_EQ("900", lt);
}

TEST(RegistrationClient, FUP_DS_10_set_endpoint_flags_pending_reregister) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(), store);
    EXPECT_FALSE(cli.pending_reregister());

    cli.set_endpoint("urn:dev:NEW");
    EXPECT_TRUE(cli.pending_reregister());
    EXPECT_EQ("urn:dev:NEW", cli.endpoint());
}

TEST(RegistrationClient, FUP_DS_10_set_endpoint_idempotent_does_not_flag) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(), store);  // ep = urn:dev:client-1
    cli.set_endpoint("urn:dev:client-1");          // unchanged
    EXPECT_FALSE(cli.pending_reregister());
}

TEST(RegistrationClient, FUP_DS_10_build_register_uses_live_endpoint) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(), store);
    cli.set_endpoint("urn:dev:MUTATED");

    auto bytes = cli.build_register_request(0x4321, std::string{0x33});
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(bytes, parsed);

    std::string ep;
    for (const auto& opt : parsed.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Query" &&
            opt.optionvalue.compare(0, 3, "ep=") == 0) {
            ep = opt.optionvalue.substr(3);
        }
    }
    EXPECT_EQ("urn:dev:MUTATED", ep);
}

TEST(RegistrationClient, FUP_DS_10_clear_pending_reregister_does_not_revert_endpoint) {
    auto store = minimal_store();
    RegistrationClient cli(minimal_cfg(), store);
    cli.set_endpoint("urn:dev:NEW");
    cli.clear_pending_reregister();
    EXPECT_FALSE(cli.pending_reregister());
    EXPECT_EQ("urn:dev:NEW", cli.endpoint());
}

/* ─────────────────────────── ack-timeout recovery ────────────────────────
 * The FSM leaves an Awaiting*Ack state only on a response, so a dropped
 * datagram once stranded it forever ("registered" on-device, expired at the
 * cloud). check_ack_timeout() retransmits a lost Update within budget, then
 * escalates to a full re-Register. */

using AR = RegistrationClient::AckRecovery;

TEST(RegistrationClient, ack_timeout_noop_while_registered_or_in_window) {
    auto store = minimal_store();
    auto cfg = minimal_cfg(/*lt*/ 100);
    cfg.ackTimeoutSeconds = 6;
    RegistrationClient cli(cfg, store);
    Harness h;
    h.feed_register_ack(cli);
    ASSERT_EQ(RegistrationState::Registered, cli.state());

    // Not awaiting anything → never fires, even far in the future.
    EXPECT_EQ(AR::None, cli.check_ack_timeout(
                            std::chrono::steady_clock::now() +
                            std::chrono::seconds(100000)));
    EXPECT_EQ(RegistrationState::Registered, cli.state());

    // Awaiting an Update but still inside the ack window → no action.
    auto t = std::chrono::steady_clock::now();
    (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
    ASSERT_EQ(RegistrationState::AwaitingUpdateAck, cli.state());
    EXPECT_EQ(AR::None, cli.check_ack_timeout(t + std::chrono::seconds(3)));
    EXPECT_EQ(RegistrationState::AwaitingUpdateAck, cli.state());
}

TEST(RegistrationClient, ack_timeout_retransmits_lost_update_within_budget) {
    auto store = minimal_store();
    auto cfg = minimal_cfg(/*lt*/ 100);
    cfg.ackTimeoutSeconds = 6;
    cfg.maxAckRetransmits = 3;
    RegistrationClient cli(cfg, store);
    Harness h;
    h.feed_register_ack(cli);

    auto t = std::chrono::steady_clock::now();
    (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
    ASSERT_EQ(RegistrationState::AwaitingUpdateAck, cli.state());

    // Past the window: retransmit, and the FSM reverts to Registered so the
    // caller's build_update_request() can re-arm it.
    EXPECT_EQ(AR::RetransmitUpdate,
              cli.check_ack_timeout(t + std::chrono::seconds(7)));
    EXPECT_EQ(RegistrationState::Registered, cli.state());
}

TEST(RegistrationClient, ack_timeout_escalates_to_reregister_after_budget) {
    auto store = minimal_store();
    auto cfg = minimal_cfg(/*lt*/ 100);
    cfg.ackTimeoutSeconds = 6;
    cfg.maxAckRetransmits = 2;
    RegistrationClient cli(cfg, store);
    Harness h;
    h.feed_register_ack(cli);

    // Burn the retransmit budget: each lost Update is retransmitted.
    for (int i = 0; i < 2; ++i) {
        auto t = std::chrono::steady_clock::now();
        (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
        ASSERT_EQ(RegistrationState::AwaitingUpdateAck, cli.state());
        EXPECT_EQ(AR::RetransmitUpdate,
                  cli.check_ack_timeout(t + std::chrono::seconds(7)));
        EXPECT_EQ(RegistrationState::Registered, cli.state());
    }

    // Budget spent → the next lost Update escalates to a full re-Register:
    // Unregistered, location cleared (caller replays bootstrap).
    auto t = std::chrono::steady_clock::now();
    (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
    EXPECT_EQ(AR::ReRegister, cli.check_ack_timeout(t + std::chrono::seconds(7)));
    EXPECT_EQ(RegistrationState::Unregistered, cli.state());
    EXPECT_TRUE(cli.location().empty());
}

TEST(RegistrationClient, ack_received_resets_retransmit_budget) {
    auto store = minimal_store();
    auto cfg = minimal_cfg(/*lt*/ 100);
    cfg.ackTimeoutSeconds = 6;
    cfg.maxAckRetransmits = 1;
    RegistrationClient cli(cfg, store);
    Harness h;
    h.feed_register_ack(cli);

    // Use up the single retransmit...
    auto t = std::chrono::steady_clock::now();
    (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
    EXPECT_EQ(AR::RetransmitUpdate,
              cli.check_ack_timeout(t + std::chrono::seconds(7)));

    // ...but a real Update ack now lands, which must reset the budget.
    h.feed_update_ack(cli);
    ASSERT_EQ(RegistrationState::Registered, cli.state());

    // Next lost Update gets a fresh retransmit (not an immediate escalation).
    auto t2 = std::chrono::steady_clock::now();
    (void)cli.build_update_request(2, std::string{0x02}, /*adv*/ false);
    EXPECT_EQ(AR::RetransmitUpdate,
              cli.check_ack_timeout(t2 + std::chrono::seconds(7)));
}

TEST(RegistrationClient, ack_timeout_on_lost_register_reregisters) {
    auto store = minimal_store();
    auto cfg = minimal_cfg();
    cfg.ackTimeoutSeconds = 6;
    RegistrationClient cli(cfg, store);

    auto t = std::chrono::steady_clock::now();
    (void)cli.build_register_request(1, std::string{0x01});
    ASSERT_EQ(RegistrationState::AwaitingRegisterAck, cli.state());

    EXPECT_EQ(AR::None, cli.check_ack_timeout(t + std::chrono::seconds(3)));
    EXPECT_EQ(AR::ReRegister, cli.check_ack_timeout(t + std::chrono::seconds(7)));
    EXPECT_EQ(RegistrationState::Unregistered, cli.state());
}

TEST(RegistrationClient, deregister_send_is_stamped_against_instant_timeout) {
    auto store = minimal_store();
    auto cfg = minimal_cfg();
    cfg.ackTimeoutSeconds = 6;
    RegistrationClient cli(cfg, store);
    Harness h;
    h.feed_register_ack(cli);

    auto t = std::chrono::steady_clock::now();
    (void)cli.build_deregister_request(2, std::string{0x02});
    ASSERT_EQ(RegistrationState::AwaitingDeregisterAck, cli.state());

    // Deregister stamps the send time, so it does NOT fire on the next tick.
    EXPECT_EQ(AR::None, cli.check_ack_timeout(t + std::chrono::seconds(3)));
    // ...but a lost Deregister ack still recovers via re-Register.
    EXPECT_EQ(AR::ReRegister, cli.check_ack_timeout(t + std::chrono::seconds(7)));
    EXPECT_EQ(RegistrationState::Unregistered, cli.state());
}
