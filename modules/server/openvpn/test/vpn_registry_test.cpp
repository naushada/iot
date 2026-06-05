/// L21/D2 — VPN Registry unit tests (TDD).
///
/// Tests the IP + proxy-port allocator for the multi-tenant cloud
/// OpenVPN server.  No data-store dependency — pure unit tests.

#include <gtest/gtest.h>

#include "vpn_registry.hpp"

using server::openvpn::VpnRegistry;

namespace {

// ── 1. Default subnet + port range ──────────────────────────────────

TEST(VpnRegistryTest, DefaultConfig) {
    VpnRegistry reg;
    EXPECT_EQ(reg.subnet(), "10.9.0.0/24");
    EXPECT_EQ(reg.proxy_port_start(), 5001U);
}

// ── 2. Custom subnet ────────────────────────────────────────────────

TEST(VpnRegistryTest, CustomSubnet) {
    VpnRegistry reg("172.16.0.0/28");  // 14 usable IPs
    EXPECT_EQ(reg.subnet(), "172.16.0.0/28");
}

// ── 3. First allocation gets .2 ─────────────────────────────────────

TEST(VpnRegistryTest, FirstIpIsGatewayPlusOne) {
    VpnRegistry reg;
    auto ip = reg.allocate_ip();
    ASSERT_TRUE(ip.has_value());
    EXPECT_EQ(*ip, "10.9.0.2");  // .1 is the VPN server gateway
}

// ── 4. Sequential allocation ────────────────────────────────────────

TEST(VpnRegistryTest, SequentialAllocation) {
    VpnRegistry reg;
    EXPECT_EQ(*reg.allocate_ip(), "10.9.0.2");
    EXPECT_EQ(*reg.allocate_ip(), "10.9.0.3");
    EXPECT_EQ(*reg.allocate_ip(), "10.9.0.4");
    EXPECT_EQ(reg.allocated_count(), 3U);
}

// ── 5. Release IP makes it reusable ─────────────────────────────────

TEST(VpnRegistryTest, ReleaseIpReusable) {
    VpnRegistry reg;
    auto ip = reg.allocate_ip();  // .2
    ASSERT_TRUE(ip.has_value());
    reg.release_ip(*ip);
    EXPECT_EQ(reg.allocated_count(), 0U);

    // Re-allocate — should get .2 again
    EXPECT_EQ(*reg.allocate_ip(), "10.9.0.2");
}

// ── 6. Port allocation is sequential ────────────────────────────────

TEST(VpnRegistryTest, PortAllocation) {
    VpnRegistry reg;
    EXPECT_EQ(*reg.allocate_port(), 5001U);
    EXPECT_EQ(*reg.allocate_port(), 5002U);
}

// ── 7. Release port makes it reusable ───────────────────────────────

TEST(VpnRegistryTest, ReleasePortReusable) {
    VpnRegistry reg;
    auto p = reg.allocate_port();  // 5001
    ASSERT_TRUE(p.has_value());
    reg.release_port(*p);
    EXPECT_EQ(*reg.allocate_port(), 5001U);
}

// ── 8. Subnet exhaustion ────────────────────────────────────────────

TEST(VpnRegistryTest, SubnetExhaustion) {
    VpnRegistry reg("10.9.0.0/30");  // 2 usable IPs: .2, .3 (only .2 allocatable after gateway at .1)
    auto ip1 = reg.allocate_ip();     // .2
    ASSERT_TRUE(ip1.has_value());
    auto ip2 = reg.allocate_ip();     // .3
    ASSERT_TRUE(ip2.has_value());
    auto ip3 = reg.allocate_ip();     // exhausted
    EXPECT_FALSE(ip3.has_value());
}

// ── 9. Port exhaustion ──────────────────────────────────────────────

TEST(VpnRegistryTest, PortExhaustion) {
    VpnRegistry reg("10.9.0.0/24", 5001, 5002);  // 2 ports: 5001, 5002
    ASSERT_TRUE(reg.allocate_port().has_value());
    ASSERT_TRUE(reg.allocate_port().has_value());
    EXPECT_FALSE(reg.allocate_port().has_value());
}

// ── 10. Allocate both IP + port together ────────────────────────────

TEST(VpnRegistryTest, AllocateBoth) {
    VpnRegistry reg;
    auto result = reg.allocate("urn:dev:gateway-1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->tun_ip, "10.9.0.2");
    EXPECT_EQ(result->proxy_port, 5001U);
    EXPECT_EQ(result->ep, "urn:dev:gateway-1");
}

// ── 11. Release both IP + port ──────────────────────────────────────

TEST(VpnRegistryTest, ReleaseBoth) {
    VpnRegistry reg;
    auto result = reg.allocate("urn:dev:gateway-1");
    ASSERT_TRUE(result.has_value());
    reg.release("urn:dev:gateway-1");
    // IP and port should be reusable
    auto next = reg.allocate("urn:dev:gateway-2");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->tun_ip, "10.9.0.2");
    EXPECT_EQ(next->proxy_port, 5001U);
}

// ── 12. Check if subnet contains an IP ──────────────────────────────

TEST(VpnRegistryTest, ContainsIp) {
    VpnRegistry reg("10.9.0.0/24");
    EXPECT_TRUE(reg.contains_ip("10.9.0.50"));
    EXPECT_FALSE(reg.contains_ip("192.168.1.1"));
    EXPECT_FALSE(reg.contains_ip("10.9.0.1"));    // gateway
    EXPECT_FALSE(reg.contains_ip("10.9.0.255"));   // broadcast
}

} // namespace
