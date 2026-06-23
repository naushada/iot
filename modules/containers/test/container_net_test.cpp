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
    EXPECT_EQ(rs.find("iot_router"), std::string::npos);
    // Masquerade for the subnet leaving any non-bridge interface.
    EXPECT_NE(rs.find("ip saddr 10.88.0.0/24 oifname != \"iot-cni0\" masquerade"),
              std::string::npos);
    EXPECT_NE(rs.find("type nat hook postrouting"), std::string::npos);
}
