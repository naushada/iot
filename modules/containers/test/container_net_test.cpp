#include <gtest/gtest.h>

#include "container_net.hpp"

using namespace containers;

TEST(NetPlan, DefaultSlash24) {
    auto p = plan_bridge_net("10.88.0.0/24", "iot-cni0");
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.bridge, "iot-cni0");
    EXPECT_EQ(p.prefix, 24);
    EXPECT_EQ(p.gateway, "10.88.0.1");
    EXPECT_EQ(p.container_ip, "10.88.0.2");
}

TEST(NetPlan, OtherSubnet) {
    auto p = plan_bridge_net("192.168.50.0/24", "br0");
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.gateway, "192.168.50.1");
    EXPECT_EQ(p.container_ip, "192.168.50.2");
}

TEST(NetPlan, HostOctetAssignsDistinctIPs) {
    // Multi-container: each running container gets a distinct .N on the bridge.
    EXPECT_EQ(plan_bridge_net("10.88.0.0/24", "br0").container_ip,      "10.88.0.2");  // default
    EXPECT_EQ(plan_bridge_net("10.88.0.0/24", "br0", 2).container_ip,   "10.88.0.2");
    EXPECT_EQ(plan_bridge_net("10.88.0.0/24", "br0", 3).container_ip,   "10.88.0.3");
    EXPECT_EQ(plan_bridge_net("10.88.0.0/24", "br0", 254).container_ip, "10.88.0.254");
    EXPECT_EQ(plan_bridge_net("10.88.0.0/24", "br0", 7).gateway,        "10.88.0.1");  // gw fixed
}

TEST(NetPlan, RejectsOutOfRangeOctet) {
    EXPECT_FALSE(plan_bridge_net("10.88.0.0/24", "br0", 1).ok);    // .1 is the gateway
    EXPECT_FALSE(plan_bridge_net("10.88.0.0/24", "br0", 0).ok);
    EXPECT_FALSE(plan_bridge_net("10.88.0.0/24", "br0", 255).ok);  // broadcast / out of range
    EXPECT_FALSE(plan_bridge_net("10.88.0.0/24", "br0", 999).ok);
}

TEST(NetPlan, RejectsNonSlash24) {
    EXPECT_FALSE(plan_bridge_net("10.0.0.0/8", "br0").ok);
    EXPECT_FALSE(plan_bridge_net("10.88.0.0/16", "br0").ok);
}

TEST(NetPlan, RejectsMalformed) {
    EXPECT_FALSE(plan_bridge_net("not-a-cidr", "br0").ok);
    EXPECT_FALSE(plan_bridge_net("10.88.0.0", "br0").ok);          // no prefix
    EXPECT_FALSE(plan_bridge_net("10.88.0.999/24", "br0").ok);     // octet > 255
    EXPECT_FALSE(plan_bridge_net("10.88.0/24", "br0").ok);         // 3 octets
}

TEST(NftRuleset, ScopedTableWithMasquerade) {
    auto rs = nft_container_ruleset("10.88.0.0/24", "iot-cni0");
    // Own scoped table — must NOT touch net-router's iot_router table.
    EXPECT_NE(rs.find("flush table inet iot_containers"), std::string::npos);
    EXPECT_NE(rs.find("table inet iot_containers {"), std::string::npos);
    // `add table` MUST precede `flush table` so the first container run (no table
    // yet) doesn't die on "flush table … No such file or directory".
    EXPECT_NE(rs.find("add table inet iot_containers"), std::string::npos);
    EXPECT_LT(rs.find("add table inet iot_containers"),
              rs.find("flush table inet iot_containers"));
    EXPECT_EQ(rs.find("iot_router"), std::string::npos);
    // Masquerade for the subnet leaving any non-bridge interface.
    EXPECT_NE(rs.find("ip saddr 10.88.0.0/24 oifname != \"iot-cni0\" masquerade"),
              std::string::npos);
    EXPECT_NE(rs.find("type nat hook postrouting"), std::string::npos);
}
