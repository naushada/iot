#include <gtest/gtest.h>

#include "coap_adapter.hpp"
#include "lwm2m_dm_client.hpp"
#include "lwm2m_dm_server.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_observe.hpp"

using namespace ::lwm2m;
namespace dmsrv = ::lwm2m::dmsrv;

namespace {

std::shared_ptr<ObjectStore> make_store_with_resource_13(std::string& live) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor d; d.oid = 3; d.urn = "urn:oma:lwm2m:oma:3:1.1";
    ObjectInstance i; i.iid = 0;
    Resource r;
    r.rid = 13; r.type = ResourceType::Integer; r.ops = Operations::R;
    r.observable = true;
    r.read = [&]() { return live; };
    i.resources[13] = r;
    d.instances[0] = i;
    store->add_object(d);
    return store;
}

CoAPAdapter::CoAPMessage make_observe_register(std::uint32_t oid,
                                               std::uint32_t iid,
                                               std::uint32_t rid,
                                               const std::string& token) {
    CoAPAdapter::CoAPMessage m;
    m.coapheader.ver = 1; m.coapheader.type = 0;
    m.coapheader.code = 1;            // GET
    m.coapheader.msgid = 0xAABB;
    m.coapheader.tokenlength = token.size();
    m.tokens.assign(token.begin(), token.end());

    // Observe option (delta 6, length 0 = register).
    CoAPAdapter::CoAPOptions obs;
    obs.optiondelta = 6; obs.optionlength = 0; obs.optionvalue = "";
    m.uripath.push_back(obs);

    auto pushPath = [&](const std::string& seg) {
        CoAPAdapter::CoAPOptions p;
        p.optiondelta = 11; p.optionlength = seg.size(); p.optionvalue = seg;
        m.uripath.push_back(p);
    };
    pushPath(std::to_string(oid));
    pushPath(std::to_string(iid));
    pushPath(std::to_string(rid));
    return m;
}

CoAPAdapter::CoAPMessage make_observe_cancel(const std::string& token) {
    CoAPAdapter::CoAPMessage m;
    m.coapheader.ver = 1; m.coapheader.type = 0; m.coapheader.code = 1;
    m.coapheader.msgid = 0xCCCC;
    m.coapheader.tokenlength = token.size();
    m.tokens.assign(token.begin(), token.end());

    CoAPAdapter::CoAPOptions obs;
    obs.optiondelta = 6; obs.optionlength = 1; obs.optionvalue.push_back(1);
    m.uripath.push_back(obs);

    CoAPAdapter::CoAPOptions p;
    p.optiondelta = 11; p.optionlength = 1; p.optionvalue = "3";
    m.uripath.push_back(p);
    return m;
}

std::uint8_t code_of(const std::string& bytes) {
    return bytes.size() >= 2 ? static_cast<std::uint8_t>(bytes[1]) : 0;
}

bool frame_is_con(const std::string& bytes) {
    if (bytes.empty()) return false;
    std::uint8_t b0 = bytes[0];
    return ((b0 >> 4) & 0x03) == 0;     // TYPE_CON
}

bool frame_is_non(const std::string& bytes) {
    if (bytes.empty()) return false;
    std::uint8_t b0 = bytes[0];
    return ((b0 >> 4) & 0x03) == 1;     // TYPE_NON
}

} // namespace

/* ─────────────────────────── REQ-IR-001 ───────────────────────────────── */

TEST(Observe, REQ_IR_001_register_returns_current_value_and_seq_zero) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("10.0.0.5:56830");
    CoAPAdapter coap;

    auto msg = make_observe_register(3, 0, 13, "T1");
    auto out = dm.handle(msg, coap);
    EXPECT_EQ(DmOutcome::Observe, out.kind);
    EXPECT_EQ(0x45, code_of(out.response));         // 2.05 Content
    EXPECT_EQ(1u, dm.observers().size());

    auto* obs = dm.observers().find("10.0.0.5:56830", "T1");
    ASSERT_NE(nullptr, obs);
    EXPECT_EQ(0u, obs->seq);
    EXPECT_TRUE(obs->lastValue.has_value());
    EXPECT_EQ("10", *obs->lastValue);
}

/* ─────────────────────────── REQ-IR-002 ───────────────────────────────── */

TEST(Observe, REQ_IR_002_value_changed_emits_notify) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;

    auto msg = make_observe_register(3, 0, 13, "TKN");
    dm.handle(msg, coap);

    live = "15";
    auto frames = dm.on_resource_changed(3, 0, 13, live);
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(0x45, code_of(frames[0]));     // 2.05 Content
    EXPECT_TRUE(frame_is_non(frames[0]));    // D4: default NON

    auto* obs = dm.observers().find("p:1", "TKN");
    ASSERT_NE(nullptr, obs);
    EXPECT_EQ(1u, obs->seq);
    EXPECT_EQ(1u, obs->notifyCount);
    EXPECT_EQ("15", *obs->lastValue);
}

TEST(Observe, REQ_IR_002_no_notify_when_value_unchanged) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;

    auto msg = make_observe_register(3, 0, 13, "T");
    dm.handle(msg, coap);

    auto frames = dm.on_resource_changed(3, 0, 13, live);  // same value
    EXPECT_TRUE(frames.empty());
}

/* ─────────────────────────── REQ-IR-003 pmin / pmax ─────────────────── */

TEST(Observe, REQ_IR_003_pmin_defers_notify) {
    NotificationAttributes a; a.shortServerId = 1; a.pmin = 30;

    ObserverContext ctx;
    ctx.shortServerId = 1;
    ctx.peer  = "p"; ctx.token = "T";
    ctx.oid = 3; ctx.iid = 0; ctx.rid = 13;
    ctx.lastValue = "10";
    ctx.attrs = a;
    auto now = std::chrono::steady_clock::now();
    ctx.lastSentAt = now;

    std::string nv = "15";
    EngineInput in; in.observer = &ctx; in.newValue = &nv;
    in.now = now + std::chrono::seconds(5);   // only 5 s elapsed
    EXPECT_EQ(NotifyDecision::Defer, evaluate(in));

    in.now = now + std::chrono::seconds(31);   // past pmin
    EXPECT_EQ(NotifyDecision::EmitNow, evaluate(in));
}

TEST(Observe, REQ_IR_003_pmax_forces_notify_via_tick) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;

    auto msg = make_observe_register(3, 0, 13, "T");
    dm.handle(msg, coap);

    // Inject pmax = 60 on the observer (Write-Attributes path would do
    // this; we set it directly so the test is hermetic).
    auto* obs = dm.observers().find("p:1", "T");
    ASSERT_NE(nullptr, obs);
    obs->attrs.pmax = 60;
    auto t0 = obs->lastSentAt;

    EXPECT_TRUE(dm.tick(t0 + std::chrono::seconds(30)).empty());
    auto frames = dm.tick(t0 + std::chrono::seconds(61));
    ASSERT_EQ(1u, frames.size());
}

/* ─────────────────────────── REQ-IR-004 thresholds ───────────────────── */

TEST(Observe, REQ_IR_004_gt_crossing_fires) {
    NotificationAttributes a; a.shortServerId = 1;
    a.hasGt = true; a.gt = 20.0;

    ObserverContext ctx;
    ctx.lastValue = "15";
    ctx.attrs = a;
    auto now = std::chrono::steady_clock::now();
    ctx.lastSentAt = now;

    std::string below = "18";  EngineInput in1{&ctx, &below, now + std::chrono::seconds(60)};
    EXPECT_EQ(NotifyDecision::Skip, evaluate(in1));

    std::string above = "25";  EngineInput in2{&ctx, &above, now + std::chrono::seconds(60)};
    EXPECT_EQ(NotifyDecision::EmitNow, evaluate(in2));
}

TEST(Observe, REQ_IR_004_st_step_fires_only_after_delta) {
    NotificationAttributes a; a.shortServerId = 1;
    a.hasSt = true; a.st = 5.0;

    ObserverContext ctx;
    ctx.lastValue = "100";
    ctx.attrs = a;
    auto now = std::chrono::steady_clock::now();
    ctx.lastSentAt = now;

    std::string small = "102";  EngineInput a1{&ctx, &small, now + std::chrono::seconds(60)};
    EXPECT_EQ(NotifyDecision::Skip, evaluate(a1));

    std::string big = "106";   EngineInput a2{&ctx, &big, now + std::chrono::seconds(60)};
    EXPECT_EQ(NotifyDecision::EmitNow, evaluate(a2));
}

TEST(Observe, REQ_IR_004_non_numeric_fallback_to_any_change) {
    NotificationAttributes a; a.shortServerId = 1;
    a.hasGt = true; a.gt = 1.0;        // numeric attribute on a string value
    ObserverContext ctx;
    ctx.lastValue = "hello";
    ctx.attrs = a;
    auto now = std::chrono::steady_clock::now();
    ctx.lastSentAt = now;

    std::string nv = "world"; EngineInput in{&ctx, &nv, now + std::chrono::seconds(60)};
    EXPECT_EQ(NotifyDecision::EmitNow, evaluate(in));
}

/* ─────────────────────────── REQ-IR-005 cancel ───────────────────────── */

TEST(Observe, REQ_IR_005_cancel_via_observe_1) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;

    auto reg = make_observe_register(3, 0, 13, "T");
    dm.handle(reg, coap);
    EXPECT_EQ(1u, dm.observers().size());

    auto cancel = make_observe_cancel("T");
    auto out = dm.handle(cancel, coap);
    EXPECT_EQ(DmOutcome::ObserveCancel, out.kind);
    EXPECT_EQ(0x45, code_of(out.response));
    EXPECT_EQ(0u, dm.observers().size());
}

TEST(Observe, REQ_IR_005_rst_drops_all_observers_for_peer) {
    std::string live = "10";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:A");
    CoAPAdapter coap;

    dm.handle(make_observe_register(3, 0, 13, "T1"), coap);
    dm.handle(make_observe_register(3, 0, 13, "T2"), coap);

    dm.calling_peer("p:B");
    dm.handle(make_observe_register(3, 0, 13, "T3"), coap);

    EXPECT_EQ(3u, dm.observers().size());
    EXPECT_EQ(2u, dm.on_rst_from("p:A"));
    EXPECT_EQ(1u, dm.observers().size());
}

/* ─────────────────────────── D4 NON / CON cadence ────────────────────── */

TEST(Observe, D4_every_10th_notify_is_con) {
    std::string live = "0";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;
    dm.handle(make_observe_register(3, 0, 13, "T"), coap);

    int conCount = 0, nonCount = 0;
    for (int v = 1; v <= 11; ++v) {
        live = std::to_string(v);
        auto frames = dm.on_resource_changed(3, 0, 13, live);
        ASSERT_EQ(1u, frames.size());
        if (frame_is_con(frames[0])) ++conCount;
        else if (frame_is_non(frames[0])) ++nonCount;
    }
    // 11 emissions; the 10th promoted to CON; rest NON.
    EXPECT_EQ(1, conCount);
    EXPECT_EQ(10, nonCount);
}

TEST(Observe, D4_critical_observation_always_con) {
    std::string live = "0";
    auto store = make_store_with_resource_13(live);
    DmClient dm(store);
    dm.calling_peer("p:1");
    CoAPAdapter coap;
    dm.handle(make_observe_register(3, 0, 13, "T"), coap);
    auto* obs = dm.observers().find("p:1", "T");
    ASSERT_NE(nullptr, obs);
    obs->observeCritical = true;

    live = "1";
    auto frames = dm.on_resource_changed(3, 0, 13, live);
    ASSERT_EQ(1u, frames.size());
    EXPECT_TRUE(frame_is_con(frames[0]));
}

/* ─────────────────────────── Registry plumbing ───────────────────────── */

TEST(ObserverRegistry, lookup_and_remove_roundtrip) {
    ObserverRegistry reg;
    ObserverContext a;  a.peer = "x"; a.token = "T";
    a.oid = 3; a.iid = 0; a.rid = 13; a.hasIid = a.hasRid = true;
    reg.add(a);
    EXPECT_NE(nullptr, reg.find("x", "T"));
    EXPECT_EQ(1u, reg.targeting(3, 0, 13).size());
    EXPECT_EQ(0u, reg.targeting(3, 0, 17).size());
    EXPECT_TRUE(reg.remove("x", "T"));
    EXPECT_EQ(nullptr, reg.find("x", "T"));
}

TEST(ObserverRegistry, whole_instance_observer_covers_any_rid) {
    ObserverRegistry reg;
    ObserverContext a; a.peer = "x"; a.token = "T";
    a.oid = 3; a.iid = 0;
    a.hasIid = true; a.hasRid = false;
    reg.add(a);
    EXPECT_EQ(1u, reg.targeting(3, 0, 17).size());
    EXPECT_EQ(1u, reg.targeting(3, 0, 99).size());
    EXPECT_EQ(0u, reg.targeting(3, 1, 17).size());
}
