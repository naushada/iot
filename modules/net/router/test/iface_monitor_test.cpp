/// Tests for iface_monitor — fake shell runner returns canned
/// `ip -j …` output so nothing touches the host's network stack.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "iface_monitor.hpp"
#include "shell.hpp"

using net_router::iface::pick_active;
using net_router::iface::probe;
using net_router::iface::probe_all;
using net_router::iface::State;
using net_router::shell::Runner;

namespace {

/// Fake runner: programmable per-command response, records every
/// argv it sees.
class FakeRunner {
public:
    struct Resp { std::string stdout_; int rc = 0; };

    void on(const std::vector<std::string>& argv, std::string body, int rc = 0) {
        table_[signature(argv)] = Resp{std::move(body), rc};
    }

    Runner make() {
        return [this](const std::vector<std::string>& argv, int* exit_code) {
            calls.push_back(argv);
            const auto key = signature(argv);
            auto it = table_.find(key);
            if (it == table_.end()) {
                if (exit_code) *exit_code = 1;
                return std::string{};
            }
            if (exit_code) *exit_code = it->second.rc;
            return it->second.stdout_;
        };
    }

    std::vector<std::vector<std::string>> calls;

private:
public:
    static std::string signature(const std::vector<std::string>& argv) {
        std::string s;
        for (const auto& a : argv) { s += a; s += '\0'; }
        return s;
    }

private:
    std::map<std::string, Resp> table_;
};

const char* kEth0Up = R"([{
  "ifindex": 2, "ifname": "eth0",
  "flags": ["BROADCAST","MULTICAST","UP","LOWER_UP"],
  "operstate": "UP"
}])";

const char* kEth0Down = R"([{
  "ifindex": 2, "ifname": "eth0",
  "flags": ["BROADCAST","MULTICAST"],
  "operstate": "DOWN"
}])";

const char* kTun0Unknown = R"([{
  "ifindex": 5, "ifname": "tun0",
  "flags": ["POINTOPOINT","MULTICAST","NOARP","UP","LOWER_UP"],
  "operstate": "UNKNOWN"
}])";

const char* kDefRouteEth0 = R"([{
  "dst": "default", "gateway": "192.168.1.1",
  "dev": "eth0", "protocol": "dhcp"
}])";

} // namespace

/* ─────────────────────── probe() ─────────────────────────────── */

TEST(IfaceProbe, ReturnsZeroInitForEmptyName) {
    FakeRunner fr;
    auto s = probe("", fr.make());
    EXPECT_FALSE(s.present);
    EXPECT_FALSE(s.up);
    EXPECT_TRUE(s.gateway.empty());
    EXPECT_TRUE(fr.calls.empty());      // never shells out
}

TEST(IfaceProbe, UpInterfaceWithGatewayPopulated) {
    FakeRunner fr;
    fr.on({"ip","-j","link","show","eth0"}, kEth0Up);
    fr.on({"ip","-j","route","show","default","dev","eth0"}, kDefRouteEth0);
    auto s = probe("eth0", fr.make());
    EXPECT_TRUE(s.present);
    EXPECT_TRUE(s.up);
    EXPECT_EQ("192.168.1.1", s.gateway);
    EXPECT_EQ("eth0", s.name);
    ASSERT_EQ(2u, fr.calls.size());     // link + route
}

TEST(IfaceProbe, DownInterfaceMarkedNotUp) {
    FakeRunner fr;
    fr.on({"ip","-j","link","show","eth0"}, kEth0Down);
    fr.on({"ip","-j","route","show","default","dev","eth0"}, "[]");
    auto s = probe("eth0", fr.make());
    EXPECT_TRUE(s.present);
    EXPECT_FALSE(s.up);
    EXPECT_TRUE(s.gateway.empty());
}

TEST(IfaceProbe, OperstateUnknownStillUpWhenFlagsHaveUpAndLowerUp) {
    // tun devices typically report operstate=UNKNOWN even when
    // running — the UP+LOWER_UP flag fallback should catch them.
    FakeRunner fr;
    fr.on({"ip","-j","link","show","tun0"}, kTun0Unknown);
    fr.on({"ip","-j","route","show","default","dev","tun0"}, "[]");
    auto s = probe("tun0", fr.make());
    EXPECT_TRUE(s.present);
    EXPECT_TRUE(s.up);
}

TEST(IfaceProbe, MissingInterfaceNonZeroExitYieldsNotPresent) {
    FakeRunner fr;     // no `on()` calls → runner returns rc=1
    auto s = probe("does-not-exist", fr.make());
    EXPECT_FALSE(s.present);
    EXPECT_FALSE(s.up);
    // Confirms we bail out after the link probe — no route call.
    EXPECT_EQ(1u, fr.calls.size());
}

TEST(IfaceProbe, GarbageJsonLeavesStateUntouched) {
    FakeRunner fr;
    fr.on({"ip","-j","link","show","eth0"}, "not-json");
    fr.on({"ip","-j","route","show","default","dev","eth0"}, "{}");
    auto s = probe("eth0", fr.make());
    // rc=0 but JSON parse fails → present stays false.
    EXPECT_FALSE(s.up);
    EXPECT_TRUE(s.gateway.empty());
}

/* ─────────────────────── probe_all + pick_active ─────────────── */

TEST(PickActive, FirstUpInterfaceWins) {
    std::vector<State> ss(3);
    ss[0].name = "eth0";     ss[0].present = true; ss[0].up = false;
    ss[1].name = "wlan0";    ss[1].present = true; ss[1].up = true;
    ss[2].name = "wwan0";    ss[2].present = true; ss[2].up = true;
    auto idx = pick_active(ss);
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(1u, *idx);
}

TEST(PickActive, NoneUpReturnsNullopt) {
    std::vector<State> ss(2);
    ss[0].present = true; ss[0].up = false;
    ss[1].present = true; ss[1].up = false;
    EXPECT_FALSE(pick_active(ss).has_value());
}

TEST(PickActive, AbsentInterfaceIgnoredEvenIfUpFieldSet) {
    std::vector<State> ss(2);
    ss[0].present = false; ss[0].up = true;   // can't happen in practice
    ss[1].present = true;  ss[1].up = true;
    EXPECT_EQ(1u, *pick_active(ss));
}

TEST(ProbeAll, OneCallPerName) {
    FakeRunner fr;
    fr.on({"ip","-j","link","show","eth0"}, kEth0Up);
    fr.on({"ip","-j","route","show","default","dev","eth0"}, kDefRouteEth0);
    // wlan0 absent → link probe rc=1.
    auto v = probe_all({"eth0", "wlan0"}, fr.make());
    ASSERT_EQ(2u, v.size());
    EXPECT_TRUE(v[0].up);
    EXPECT_FALSE(v[1].present);
}
