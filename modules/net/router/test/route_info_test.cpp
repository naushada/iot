/// route_info — pure parsers for the live routing snapshot
/// (net.routes / net.ifaces / net.dns).

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "route_info.hpp"

using namespace net_router;
using json = nlohmann::json;

TEST(RouteInfo, ParsesRouteTable) {
    // Trimmed `ip -j route show` from an RPi3B with VPN up.
    const char* body = R"([
      {"dst":"default","gateway":"192.168.1.1","dev":"wlan0","protocol":"dhcp","prefsrc":"192.168.1.20","metric":600},
      {"dst":"10.9.0.0/24","dev":"tun0","protocol":"kernel","scope":"link","prefsrc":"10.9.0.6"},
      {"dst":"192.168.1.0/24","dev":"wlan0","protocol":"kernel","scope":"link","prefsrc":"192.168.1.20"}
    ])";
    const auto out = json::parse(route_info::parse_routes(body));
    ASSERT_TRUE(out.is_array());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0]["dst"], "default");
    EXPECT_EQ(out[0]["gateway"], "192.168.1.1");
    EXPECT_EQ(out[0]["dev"], "wlan0");
    EXPECT_EQ(out[0]["proto"], "dhcp");
    EXPECT_EQ(out[0]["metric"], "600");        // numeric field → string
    EXPECT_EQ(out[1]["dst"], "10.9.0.0/24");
    EXPECT_EQ(out[1]["gateway"], "");          // absent field → ""
    EXPECT_EQ(out[1]["scope"], "link");
}

TEST(RouteInfo, RouteGarbageYieldsEmptyArray) {
    EXPECT_EQ(route_info::parse_routes("not json"), "[]");
    EXPECT_EQ(route_info::parse_routes(""), "[]");
    EXPECT_EQ(route_info::parse_routes("{\"a\":1}"), "[]");   // not an array
}

TEST(RouteInfo, ParsesIfaceList) {
    const char* body = R"([
      {"ifname":"lo","operstate":"UNKNOWN","address":"00:00:00:00:00:00",
       "addr_info":[{"family":"inet","local":"127.0.0.1","prefixlen":8}]},
      {"ifname":"wlan0","operstate":"UP","address":"b8:27:eb:11:22:33",
       "addr_info":[{"family":"inet6","local":"fe80::1","prefixlen":64},
                    {"family":"inet","local":"192.168.1.20","prefixlen":24}]},
      {"ifname":"eth0","operstate":"DOWN","address":"b8:27:eb:44:55:66","addr_info":[]}
    ])";
    const auto out = json::parse(route_info::parse_ifaces(body));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1]["name"], "wlan0");
    EXPECT_EQ(out[1]["state"], "UP");
    EXPECT_EQ(out[1]["mac"], "b8:27:eb:11:22:33");
    EXPECT_EQ(out[1]["ip"], "192.168.1.20/24");   // v6 skipped, prefix kept
    EXPECT_EQ(out[2]["ip"], "");                   // no address
}

TEST(RouteInfo, IfaceGarbageYieldsEmptyArray) {
    EXPECT_EQ(route_info::parse_ifaces("nope"), "[]");
    EXPECT_EQ(route_info::parse_ifaces(""), "[]");
}

TEST(RouteInfo, ParsesResolvConf) {
    EXPECT_EQ(route_info::parse_resolv_conf(
        "# generated\nnameserver 192.168.1.1\nnameserver 8.8.8.8\nsearch lan\n"),
        "192.168.1.1,8.8.8.8");
    EXPECT_EQ(route_info::parse_resolv_conf("search lan\n"), "");
    EXPECT_EQ(route_info::parse_resolv_conf(""), "");
}
