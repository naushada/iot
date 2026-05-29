#include <gtest/gtest.h>

#include "lwm2m_bootstrap.hpp"
#include "lwm2m_bootstrap_client.hpp"
#include "lwm2m_bootstrap_server.hpp"
#include "lwm2m_object_store.hpp"
#include "coap_adapter.hpp"

namespace bs = ::lwm2m::bootstrap;
using ::lwm2m::ObjectDescriptor;
using ::lwm2m::ObjectStore;

namespace {

bs::AccountProvisioning make_account(const std::string& ep) {
    bs::AccountProvisioning a;
    a.endpoint = ep;

    bs::SecurityInstance bsAcct;
    bsAcct.iid               = 0;
    bsAcct.serverUri         = "coaps://bs.example.com:5684";
    bsAcct.isBootstrapServer = true;
    bsAcct.securityMode      = 3;   // NoSec for the test
    bsAcct.shortServerId     = 0;
    a.security.push_back(bsAcct);

    bs::SecurityInstance dmAcct;
    dmAcct.iid               = 1;
    dmAcct.serverUri         = "coaps://dm.example.com:5684";
    dmAcct.isBootstrapServer = false;
    dmAcct.securityMode      = 0;   // PSK
    dmAcct.identity          = std::string("device-identity-01");
    dmAcct.secretKey         = std::string("\x11\x22\x33\x44", 4);
    dmAcct.shortServerId     = 1;
    a.security.push_back(dmAcct);

    bs::ServerInstance srv;
    srv.iid           = 0;
    srv.shortServerId = 1;
    srv.lifetime      = 7200;
    srv.binding       = "U";
    a.server.push_back(srv);

    return a;
}

CoAPAdapter::CoAPMessage make_bs_request(const std::string& ep) {
    CoAPAdapter::CoAPMessage m;
    m.coapheader.ver         = 1;
    m.coapheader.type        = 0;
    m.coapheader.tokenlength = 1;
    m.coapheader.code        = 2;     // POST
    m.coapheader.msgid       = 0xA1B2;
    m.tokens                 = {0x77};
    CoAPAdapter::CoAPOptions path; path.optiondelta = 11; path.optionlength = 2; path.optionvalue = "bs";
    m.uripath.push_back(path);
    if (!ep.empty()) {
        std::string q = "ep=" + ep;
        CoAPAdapter::CoAPOptions query;
        query.optiondelta  = 15;
        query.optionlength = q.size();
        query.optionvalue  = q;
        m.uripath.push_back(query);
    }
    return m;
}

std::uint8_t response_code(const std::string& bytes) {
    return bytes.size() >= 2 ? static_cast<std::uint8_t>(bytes[1]) : 0;
}

} // namespace

/* ─────────────────────────── REQ-BS-006 / 007 / 008 ──────────────────── */

TEST(Bootstrap, REQ_BS_006_known_endpoint_emits_ack_puts_finish) {
    bs::Server srv;
    srv.add_account(make_account("urn:dev:client-1"));
    CoAPAdapter coap;

    auto msg = make_bs_request("urn:dev:client-1");
    auto r = srv.handle(msg, coap);

    ASSERT_TRUE(r.handled);
    // 1 ACK + 2 Security PUTs + 1 Server PUT + 1 Finish = 5 frames
    ASSERT_EQ(5u, r.frames.size());
    EXPECT_EQ(0x44, response_code(r.frames[0]));   // 2.04 Changed ACK
    EXPECT_EQ("urn:dev:client-1", r.endpoint);
}

TEST(Bootstrap, REQ_BS_006_unknown_endpoint_returns_4_04) {
    bs::Server srv;
    CoAPAdapter coap;
    auto msg = make_bs_request("urn:unknown");
    auto r = srv.handle(msg, coap);

    ASSERT_TRUE(r.handled);
    ASSERT_EQ(1u, r.frames.size());
    EXPECT_EQ(0x84, response_code(r.frames[0]));   // 4.04 Not Found
}

TEST(Bootstrap, REQ_BS_006_missing_ep_returns_4_00) {
    bs::Server srv;
    srv.add_account(make_account("urn:dev:client-1"));
    CoAPAdapter coap;
    auto msg = make_bs_request("");
    auto r = srv.handle(msg, coap);

    ASSERT_TRUE(r.handled);
    ASSERT_EQ(1u, r.frames.size());
    EXPECT_EQ(0x80, response_code(r.frames[0]));   // 4.00 Bad Request
}

TEST(Bootstrap, ignores_non_bs_uri) {
    bs::Server srv;
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage m;
    m.coapheader.code = 2;
    CoAPAdapter::CoAPOptions p; p.optiondelta = 11; p.optionlength = 2; p.optionvalue = "rd";
    m.uripath.push_back(p);

    auto r = srv.handle(m, coap);
    EXPECT_FALSE(r.handled);
    EXPECT_TRUE(r.frames.empty());
}

/* ─────────────────────────── REQ-BS-001 client request ───────────────── */

TEST(Bootstrap, REQ_BS_001_client_builds_post_bs_with_ep) {
    auto store = std::make_shared<ObjectStore>();
    bs::Client cli("urn:dev:client-1", store, /*dtls*/ nullptr);
    auto bytes = cli.build_bs_request(0xCAFE, std::string{0x01});

    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(bytes, parsed);
    EXPECT_EQ(0xCAFE, parsed.coapheader.msgid);
    EXPECT_EQ(2u, parsed.coapheader.code);

    bool sawBsUri = false, sawEpQuery = false;
    for (const auto& opt : parsed.uripath) {
        auto name = coap.getOptionNumber(opt.optiondelta);
        if (name == "Uri-Path" && opt.optionvalue == "bs") sawBsUri = true;
        if (name == "Uri-Query" && opt.optionvalue == "ep=urn:dev:client-1") sawEpQuery = true;
    }
    EXPECT_TRUE(sawBsUri);
    EXPECT_TRUE(sawEpQuery);
    EXPECT_EQ(bs::ClientState::AwaitingBSAck, cli.state());
}

/* ─────────────────────────── REQ-BS-002 + 004 e2e ────────────────────── */

TEST(Bootstrap, REQ_BS_004_end_to_end_commits_to_store) {
    auto store = std::make_shared<ObjectStore>();
    bs::Client cli("urn:dev:client-1", store, /*dtls*/ nullptr);
    bs::Server srv;
    srv.add_account(make_account("urn:dev:client-1"));
    CoAPAdapter coap;

    // 1. Client sends POST /bs.
    auto req_bytes = cli.build_bs_request(1, std::string{0x01});
    CoAPAdapter::CoAPMessage req;
    coap.parseRequest(req_bytes, req);

    // 2. Server replies with ACK + PUTs + Finish.
    auto sr = srv.handle(req, coap);
    ASSERT_TRUE(sr.handled);
    ASSERT_EQ(5u, sr.frames.size());

    // 3. Feed each frame through the client.
    for (const auto& frame : sr.frames) {
        CoAPAdapter::CoAPMessage f;
        coap.parseRequest(frame, f);
        cli.handle_bs_traffic(f, coap);
    }

    // 4. Client should be Done.
    EXPECT_EQ(bs::ClientState::Done, cli.state());

    // 5. Security & Server objects should be installed.
    ASSERT_TRUE(store->has(0));
    ASSERT_TRUE(store->has(1));
    EXPECT_TRUE(store->has(0, 0));
    EXPECT_TRUE(store->has(0, 1));
    EXPECT_TRUE(store->has(1, 0));
}

TEST(Bootstrap, REQ_BS_004_failed_commit_leaves_store_intact) {
    // If the staged set never reaches Finish, the live store is unchanged.
    auto store = std::make_shared<ObjectStore>();
    bs::Client cli("urn:dev:client-1", store, nullptr);
    bs::Server srv;
    srv.add_account(make_account("urn:dev:client-1"));
    CoAPAdapter coap;

    auto req_bytes = cli.build_bs_request(1, std::string{0x01});
    CoAPAdapter::CoAPMessage req;
    coap.parseRequest(req_bytes, req);
    auto sr = srv.handle(req, coap);

    // Drop the final Finish frame.
    sr.frames.pop_back();
    for (const auto& frame : sr.frames) {
        CoAPAdapter::CoAPMessage f;
        coap.parseRequest(frame, f);
        cli.handle_bs_traffic(f, coap);
    }
    EXPECT_NE(bs::ClientState::Done, cli.state());
    EXPECT_FALSE(store->has(0));     // nothing committed
    EXPECT_FALSE(store->has(1));
}

/* ─────────────────────────── REQ-BS-003 delete ───────────────────────── */

TEST(Bootstrap, REQ_BS_003_delete_root_purges_staging) {
    auto store = std::make_shared<ObjectStore>();
    // Pre-populate /3/0 (Device) so we can verify the purge took effect.
    ObjectDescriptor d3;  d3.oid = 3; d3.name = "Device";
    ObjectDescriptor d0;  d0.oid = 0; d0.name = "Security";
    ::lwm2m::ObjectInstance d0i; d0i.iid = 5;
    d0.instances[5] = d0i;
    store->add_object(d3);
    store->add_object(d0);

    bs::Client cli("urn:dev:client-1", store, nullptr);
    bs::Server srv;
    srv.add_account(make_account("urn:dev:client-1"));
    CoAPAdapter coap;

    // Drive into WaitForBSWrites.
    auto req_bytes = cli.build_bs_request(1, std::string{0x01});
    CoAPAdapter::CoAPMessage req;
    coap.parseRequest(req_bytes, req);
    auto sr = srv.handle(req, coap);

    // Feed the ACK so the client advances.
    {
        CoAPAdapter::CoAPMessage ack;
        coap.parseRequest(sr.frames.front(), ack);
        cli.handle_bs_traffic(ack, coap);
    }
    EXPECT_EQ(bs::ClientState::WaitForBSWrites, cli.state());

    // Synthesize a DELETE /.
    CoAPAdapter::CoAPMessage del;
    del.coapheader.ver = 1; del.coapheader.type = 0;
    del.coapheader.tokenlength = 0; del.coapheader.code = 4 /*DELETE*/;
    del.coapheader.msgid = 0x9999;
    auto rsp = cli.handle_bs_traffic(del, coap);
    EXPECT_EQ(0x42, response_code(rsp));    // 2.02 Deleted

    EXPECT_TRUE(cli.staging().purge);
}

/* ─────────────────────────── BUG-001 follow-on coverage ──────────────── */
//
// Removed from the unit-test build: instantiating DTLSAdapter pulls in
// dtls_adapter.cpp whose dtor calls dtls_free_context, requiring tinydtls
// at link time. The unit-test target deliberately doesn't link tinydtls
// (we only want to exercise pure-C++ logic here). The BUG-001 runtime
// regression is owned by the Leshan interop pass per RDD §3.10 + L9
// risk gate — `apps/docs/leshan-interop.md` §3.
