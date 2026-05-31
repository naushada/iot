/// Tests for ip_route — fake runner records `ip route replace ...`
/// invocations so we can assert command shape + skip behaviour.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "iface_monitor.hpp"
#include "ip_route.hpp"
#include "shell.hpp"

using net_router::iface::State;
using net_router::route::apply_priorities;
using net_router::shell::Runner;

namespace {

struct Recorder {
    std::vector<std::vector<std::string>> calls;
    int rc_to_return = 0;

    Runner make() {
        return [this](const std::vector<std::string>& argv, int* ec) {
            calls.push_back(argv);
            if (ec) *ec = rc_to_return;
            return std::string{};
        };
    }
};

State up(const std::string& n, const std::string& gw) {
    State s; s.name = n; s.present = true; s.up = true; s.gateway = gw;
    return s;
}
State down(const std::string& n) {
    State s; s.name = n; s.present = true; s.up = false; return s;
}
State no_gw(const std::string& n) {
    State s; s.name = n; s.present = true; s.up = true; return s;
}

} // namespace

TEST(IpRoute, MetricIsIndexPlusOneTimesHundred) {
    Recorder rec;
    auto r = apply_priorities({
        up("eth0",   "192.168.1.1"),
        up("wlan0",  "10.0.0.1"),
        up("wwan0",  "172.16.0.1"),
    }, rec.make());

    ASSERT_EQ(3u, r.steps.size());
    EXPECT_EQ(100u, r.steps[0].metric);
    EXPECT_EQ(200u, r.steps[1].metric);
    EXPECT_EQ(300u, r.steps[2].metric);
    EXPECT_TRUE(r.all_ok);
    for (const auto& s : r.steps) EXPECT_TRUE(s.applied);
}

TEST(IpRoute, IssuesIpRouteReplaceWithViaAndDevAndMetric) {
    Recorder rec;
    apply_priorities({ up("eth0", "192.168.1.1") }, rec.make());
    ASSERT_EQ(1u, rec.calls.size());
    const auto& c = rec.calls[0];
    // Expect exactly: ip route replace default via 192.168.1.1
    //                 dev eth0 metric 100
    ASSERT_EQ(10u, c.size());
    EXPECT_EQ("ip",            c[0]);
    EXPECT_EQ("route",         c[1]);
    EXPECT_EQ("replace",       c[2]);
    EXPECT_EQ("default",       c[3]);
    EXPECT_EQ("via",           c[4]);
    EXPECT_EQ("192.168.1.1",   c[5]);
    EXPECT_EQ("dev",           c[6]);
    EXPECT_EQ("eth0",          c[7]);
    EXPECT_EQ("metric",        c[8]);
    EXPECT_EQ("100",           c[9]);
}

TEST(IpRoute, SkipsDownInterfacesButContinuesWithOthers) {
    Recorder rec;
    auto r = apply_priorities({
        down("eth0"),
        up("wlan0", "10.0.0.1"),
    }, rec.make());

    ASSERT_EQ(2u, r.steps.size());
    EXPECT_FALSE(r.steps[0].applied);
    EXPECT_EQ("iface not up", r.steps[0].error);
    EXPECT_TRUE(r.steps[1].applied);
    // Down iface metric is still recorded for diagnostic completeness.
    EXPECT_EQ(100u, r.steps[0].metric);
    EXPECT_EQ(200u, r.steps[1].metric);
    EXPECT_TRUE(r.all_ok);          // skip ≠ failure

    // Only the wlan0 call actually went to the runner.
    ASSERT_EQ(1u, rec.calls.size());
    EXPECT_EQ("wlan0", rec.calls[0][7]);
}

TEST(IpRoute, NoGatewaySkipsApply) {
    Recorder rec;
    auto r = apply_priorities({ no_gw("eth0") }, rec.make());
    ASSERT_EQ(1u, r.steps.size());
    EXPECT_FALSE(r.steps[0].applied);
    EXPECT_EQ("no default gateway known", r.steps[0].error);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_TRUE(r.all_ok);
}

TEST(IpRoute, EmptyNameFlagsAllOkFalse) {
    Recorder rec;
    State s; s.present = true; s.up = true; s.gateway = "1.2.3.4";
    auto r = apply_priorities({ s }, rec.make());
    EXPECT_FALSE(r.all_ok);
    EXPECT_EQ("iface name empty", r.steps[0].error);
    EXPECT_TRUE(rec.calls.empty());
}

TEST(IpRoute, NonZeroExitMarksStepFailedAndAllOkFalse) {
    Recorder rec;
    rec.rc_to_return = 2;
    auto r = apply_priorities({ up("eth0", "192.168.1.1") }, rec.make());
    ASSERT_EQ(1u, r.steps.size());
    EXPECT_FALSE(r.steps[0].applied);
    EXPECT_EQ(2, r.steps[0].rc);
    EXPECT_FALSE(r.all_ok);
}

TEST(IpRoute, NullRunnerIsAnError) {
    auto r = apply_priorities({ up("eth0", "192.168.1.1") }, Runner{});
    EXPECT_FALSE(r.all_ok);
    EXPECT_EQ("no shell runner", r.steps[0].error);
}
