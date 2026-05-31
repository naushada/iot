/// Tests for the Lifecycle FSM. Pure: lambda fakes capture all the
/// would-be side effects, so we assert on what step() emits.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "iface_monitor.hpp"
#include "lifecycle.hpp"
#include "nft_rules.hpp"

using net_router::iface::State;
using net_router::Lifecycle;

namespace {

/// Captures every side effect the Lifecycle attempts. Defaults to
/// "all successes"; tests flip nft_ok / routes_ok to exercise the
/// error branches.
struct Capture {
    std::vector<std::string>            nft_rulesets;
    std::vector<std::vector<State>>     route_calls;
    std::vector<std::string>            state_writes;
    std::vector<std::string>            iface_writes;
    std::vector<std::uint32_t>          rules_applied_count_writes;
    std::vector<std::uint32_t>          last_apply_unix_writes;
    bool nft_ok    = true;
    bool routes_ok = true;
    std::string nft_err;

    Lifecycle::Sinks make_sinks() {
        Lifecycle::Sinks s;
        s.apply_nft = [this](const std::string& rs, std::string* err) {
            nft_rulesets.push_back(rs);
            if (!nft_ok && err) *err = nft_err.empty() ? "fake-fail" : nft_err;
            return nft_ok;
        };
        s.apply_routes = [this](const std::vector<State>& v) {
            route_calls.push_back(v);
            return routes_ok;
        };
        s.set_state              = [this](const std::string& v){ state_writes.push_back(v); };
        s.set_iface_active       = [this](const std::string& v){ iface_writes.push_back(v); };
        s.set_rules_applied_count= [this](std::uint32_t v){ rules_applied_count_writes.push_back(v); };
        s.set_last_apply_unix    = [this](std::uint32_t v){ last_apply_unix_writes.push_back(v); };
        return s;
    }
};

State up(const std::string& n, const std::string& gw = "10.0.0.1") {
    State s; s.name = n; s.present = true; s.up = true; s.gateway = gw;
    return s;
}
State down(const std::string& n) {
    State s; s.name = n; s.present = true; s.up = false; return s;
}

Lifecycle::Inputs configured(std::vector<State> ifaces = {up("eth0")}) {
    Lifecycle::Inputs in;
    in.tun_dev                  = "tun0";
    in.lwm2m_target_ip          = "192.168.1.10";
    in.lwm2m_target_port        = 5684;
    in.forward_ports            = {80, 443, 5684};
    in.ifaces_in_priority_order = std::move(ifaces);
    in.now_unix                 = 1748707200;  // 2026-05-31 00:00 UTC
    return in;
}

} // namespace

/* ─────────────── Init / NeedConfig ─────────────── */

TEST(Lifecycle, EmptyTargetIpYieldsNeedConfigAndNoApplyAttempt) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    Lifecycle::Inputs in;
    in.tun_dev = "tun0";   // target_ip deliberately empty
    in.ifaces_in_priority_order = {up("eth0")};

    EXPECT_EQ(Lifecycle::State::NeedConfig, lc.step(in));
    EXPECT_TRUE(cap.nft_rulesets.empty());
    EXPECT_TRUE(cap.route_calls.empty());
    // Init → NeedConfig emits a state write.
    ASSERT_EQ(1u, cap.state_writes.size());
    EXPECT_EQ("need-config", cap.state_writes[0]);
}

/* ─────────────── First successful apply ─────────────── */

TEST(Lifecycle, FirstStepAppliesNftAndRoutesAndReachesSteady) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    auto rc = lc.step(configured());

    EXPECT_EQ(Lifecycle::State::Steady, rc);
    EXPECT_EQ(1u, cap.nft_rulesets.size());
    EXPECT_EQ(1u, cap.route_calls.size());
    EXPECT_EQ(1u, lc.apply_count());

    // Ruleset must come from nft::build_nft_ruleset — sanity check
    // a couple of substrings the generator always emits.
    const auto& rs = cap.nft_rulesets[0];
    EXPECT_NE(std::string::npos, rs.find("flush table inet iot_router"));
    EXPECT_NE(std::string::npos, rs.find("dnat to 192.168.1.10:80"));

    ASSERT_EQ(1u, cap.rules_applied_count_writes.size());
    EXPECT_EQ(1u, cap.rules_applied_count_writes[0]);
    ASSERT_EQ(1u, cap.last_apply_unix_writes.size());
    EXPECT_EQ(1748707200u, cap.last_apply_unix_writes[0]);

    ASSERT_EQ(1u, cap.iface_writes.size());
    EXPECT_EQ("eth0", cap.iface_writes[0]);
    ASSERT_EQ(1u, cap.state_writes.size());
    EXPECT_EQ("steady", cap.state_writes[0]);
}

TEST(Lifecycle, IdenticalSecondStepDoesNotReApplyNft) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    lc.step(configured());
    lc.step(configured());          // no change

    EXPECT_EQ(1u, cap.nft_rulesets.size());          // still 1
    EXPECT_EQ(1u, cap.rules_applied_count_writes.size());
    EXPECT_EQ(2u, cap.route_calls.size());           // routes always run
    EXPECT_EQ(1u, lc.apply_count());
    // Steady → Steady: no extra state write.
    EXPECT_EQ(1u, cap.state_writes.size());
}

TEST(Lifecycle, ForwardPortsChangeTriggersReapply) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    lc.step(configured());
    auto in2 = configured();
    in2.forward_ports = {80, 443, 5684, 8883};   // added 8883
    lc.step(in2);

    EXPECT_EQ(2u, cap.nft_rulesets.size());
    EXPECT_NE(std::string::npos,
              cap.nft_rulesets[1].find("dnat to 192.168.1.10:8883"));
    EXPECT_EQ(2u, lc.apply_count());
}

/* ─────────────── pick_active interface tracking ─────────────── */

TEST(Lifecycle, IfaceFlipFromEthToWlanWritesNewActive) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    // eth up, wlan up — priority picks eth0
    lc.step(configured({up("eth0"), up("wlan0")}));
    EXPECT_EQ("eth0", lc.last_iface());

    // eth goes down — should flip to wlan
    lc.step(configured({down("eth0"), up("wlan0")}));
    EXPECT_EQ("wlan0", lc.last_iface());

    ASSERT_EQ(2u, cap.iface_writes.size());
    EXPECT_EQ("eth0",  cap.iface_writes[0]);
    EXPECT_EQ("wlan0", cap.iface_writes[1]);
}

TEST(Lifecycle, AllIfacesDownEmitsEmptyActive) {
    Capture cap;
    Lifecycle lc(cap.make_sinks());

    lc.step(configured({up("eth0")}));
    lc.step(configured({down("eth0")}));

    ASSERT_EQ(2u, cap.iface_writes.size());
    EXPECT_EQ("eth0", cap.iface_writes[0]);
    EXPECT_EQ("",     cap.iface_writes[1]);   // operator signal: offline
}

/* ─────────────── Failure + recovery ─────────────── */

TEST(Lifecycle, NftFailureTransitionsToFailedWithoutBumpingCount) {
    Capture cap;
    cap.nft_ok  = false;
    cap.nft_err = "iot-router:7: invalid type";
    Lifecycle lc(cap.make_sinks());

    auto rc = lc.step(configured());
    EXPECT_EQ(Lifecycle::State::Failed, rc);
    EXPECT_EQ(0u, lc.apply_count());            // no count bump
    EXPECT_TRUE(cap.rules_applied_count_writes.empty());
    // last_ruleset stays empty so the next clean tick will re-apply.
    EXPECT_TRUE(lc.last_ruleset().empty());
    ASSERT_EQ(1u, cap.state_writes.size());
    EXPECT_EQ("failed", cap.state_writes[0]);
}

TEST(Lifecycle, RouteFailureAloneTransitionsToFailed) {
    Capture cap;
    cap.routes_ok = false;
    Lifecycle lc(cap.make_sinks());
    auto rc = lc.step(configured());
    EXPECT_EQ(Lifecycle::State::Failed, rc);
    // nft side still succeeded → ruleset is recorded so we don't
    // re-apply it on the next tick.
    EXPECT_EQ(1u, lc.apply_count());
}

TEST(Lifecycle, FailedToSteadyEmitsSteadyStateWrite) {
    Capture cap;
    cap.nft_ok = false;
    Lifecycle lc(cap.make_sinks());

    lc.step(configured());     // → Failed
    cap.nft_ok = true;
    lc.step(configured());     // → Steady (re-applies)

    ASSERT_GE(cap.state_writes.size(), 2u);
    EXPECT_EQ("failed", cap.state_writes[0]);
    EXPECT_EQ("steady", cap.state_writes[1]);
    EXPECT_EQ(1u, lc.apply_count());
}

/* ─────────────── state_name helper ─────────────── */

TEST(Lifecycle, StateNameRoundTrip) {
    EXPECT_STREQ("init",        Lifecycle::state_name(Lifecycle::State::Init));
    EXPECT_STREQ("need-config", Lifecycle::state_name(Lifecycle::State::NeedConfig));
    EXPECT_STREQ("steady",      Lifecycle::state_name(Lifecycle::State::Steady));
    EXPECT_STREQ("failed",      Lifecycle::state_name(Lifecycle::State::Failed));
}
