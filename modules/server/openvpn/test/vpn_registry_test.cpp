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
    // A /30 spans .0-.3: .0 network, .1 gateway, .3 broadcast — leaving exactly
    // ONE allocatable host, .2. (The old comment here claimed two and the test
    // asserted two; it had been failing since it was written.)
    VpnRegistry reg("10.9.0.0/30");
    auto ip1 = reg.allocate_ip();     // .2 — the only one
    ASSERT_TRUE(ip1.has_value());
    EXPECT_EQ(*ip1, "10.9.0.2");
    auto ip2 = reg.allocate_ip();     // exhausted
    EXPECT_FALSE(ip2.has_value());
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

// ── 13. Multi-tenant: allocate_in_subnet (P3c) ──────────────────────

TEST(VpnRegistryTest, AllocateInTenantSubnet) {
    // Base pool is the legacy /24; a tenant device draws from its own /24
    // (10.9.16.0/24, OUTSIDE the base pool) but shares the proxy-port range.
    VpnRegistry reg("10.9.0.0/24", 5001, 5003);
    auto a = reg.allocate_in_subnet("dev-a", "10.9.16.0/24");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->tun_ip, "10.9.16.2");        // .0 net, .1 gw → first host .2
    EXPECT_EQ(a->proxy_port, 5001U);          // port from the shared range
    // A second tenant device gets the next free IP in its /24 + next port.
    auto b = reg.allocate_in_subnet("dev-b", "10.9.16.0/24");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->tun_ip, "10.9.16.3");
    EXPECT_EQ(b->proxy_port, 5002U);
}

TEST(VpnRegistryTest, AllocateInSubnetReDerivesAfterRelease) {
    // release() frees the tenant IP; the next allocate_in_subnet re-derives the
    // same first host (it's tracked while in use, re-derivable once freed).
    VpnRegistry reg("10.9.0.0/24");
    auto a = reg.allocate_in_subnet("dev", "10.9.16.0/24");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->tun_ip, "10.9.16.2");
    reg.release("dev");
    auto b = reg.allocate_in_subnet("dev2", "10.9.16.0/24");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->tun_ip, "10.9.16.2");        // freed → re-derived
}

TEST(VpnRegistryTest, TenantIpNotReturnedToBasePool) {
    // Releasing a tenant IP must not pollute the base /24 free pool: a later
    // base allocate() yields a base-subnet IP, never the released tenant IP.
    VpnRegistry reg("10.9.0.0/24");
    auto t = reg.allocate_in_subnet("dev", "10.9.16.0/24");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->tun_ip, "10.9.16.2");
    reg.release("dev");
    auto base = reg.allocate("base");
    ASSERT_TRUE(base.has_value());
    EXPECT_NE(base->tun_ip, "10.9.16.2");                 // not the tenant IP
    EXPECT_EQ(base->tun_ip.rfind("10.9.0.", 0), 0u);      // a base-subnet IP
}

// ── Port exhaustion is NOT fatal (tdd-cloud-scale-1m-devices §C1/P0b) ───────
//
// The proxy port only feeds the legacy per-device DNAT; the path proxy
// (/dev/<ep>/) needs no port. A dry port pool must therefore still yield a
// usable allocation (tun_ip + proxy_port == 0), NOT nullopt — returning nullopt
// made BootstrapProvisioner fail, which meant the 52nd device could not onboard
// at all (no tunnel IP, no BS PSK).

TEST(VpnRegistryTest, AllocateInSubnetPortExhaustionIsNotFatal) {
    VpnRegistry reg("10.9.0.0/24", 5001, 5001);   // exactly one port
    auto a = reg.allocate_in_subnet("a", "10.9.16.0/24");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->proxy_port, 5001U);

    auto b = reg.allocate_in_subnet("b", "10.9.16.0/24");   // port pool now dry
    ASSERT_TRUE(b.has_value()) << "a dry port pool must not block onboarding";
    EXPECT_EQ(b->proxy_port, 0U);                  // sentinel: no DNAT for this one
    EXPECT_FALSE(b->tun_ip.empty());               // …but it still gets a tunnel IP
    EXPECT_NE(b->tun_ip, a->tun_ip);
}

TEST(VpnRegistryTest, AllocateInSubnetTenantExhaustionIsStillFatal) {
    // /30 tenant subnet → exactly one allocatable host (.2). The SUBNET running
    // dry is fatal — unlike the port pool.
    VpnRegistry reg("10.9.0.0/24", 5001, 5100);
    EXPECT_TRUE(reg.allocate_in_subnet("a", "10.9.16.0/30").has_value());
    EXPECT_FALSE(reg.allocate_in_subnet("b", "10.9.16.0/30").has_value());
}

TEST(VpnRegistryTest, PortExhaustionDoesNotBlockAllocate) {
    VpnRegistry reg("10.9.0.0/24", 5001, 5002);   // exactly two ports
    auto a = reg.allocate("ep-a");
    auto b = reg.allocate("ep-b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->proxy_port, 5001U);
    EXPECT_EQ(b->proxy_port, 5002U);

    auto c = reg.allocate("ep-c");                // ports dry, IPs plentiful
    ASSERT_TRUE(c.has_value()) << "the 3rd device must still onboard";
    EXPECT_EQ(c->proxy_port, 0U);
    EXPECT_EQ(c->tun_ip, "10.9.0.4");             // IP allocation marches on

    auto d = reg.allocate("ep-d");                // and so must the 4th…
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->proxy_port, 0U);
    EXPECT_EQ(d->tun_ip, "10.9.0.5");             // …with a DISTINCT IP
}

TEST(VpnRegistryTest, IpExhaustionIsStillFatal) {
    // Ports are plentiful, IPs are not. Drain the IP pool without hard-coding
    // its exact size, then assert the NEXT allocate fails: no tunnel IP == no
    // VPN presence == genuinely cannot onboard. This is the one exhaustion that
    // must still be fatal.
    VpnRegistry reg("10.9.0.0/29", 5001, 5100);
    int n = 0;
    while (reg.allocate("ep-" + std::to_string(n)).has_value()) {
        ASSERT_LT(++n, 64) << "a /29 pool should exhaust long before this";
    }
    EXPECT_GT(n, 0) << "at least one IP must have been allocatable";
    EXPECT_FALSE(reg.allocate("ep-last").has_value());
}

TEST(VpnRegistryTest, ReleasingAPortlessEndpointDoesNotCorruptThePool) {
    VpnRegistry reg("10.9.0.0/24", 5001, 5001);   // one port
    auto a = reg.allocate("ep-a");                // takes 5001
    auto b = reg.allocate("ep-b");                // portless
    ASSERT_TRUE(a.has_value() && b.has_value());
    ASSERT_EQ(b->proxy_port, 0U);

    reg.release("ep-b");                          // releasing port 0 must be a no-op
    auto c = reg.allocate("ep-c");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->proxy_port, 0U) << "port 0 must never be recycled INTO the pool";

    reg.release("ep-a");                          // now 5001 really does come back
    auto d = reg.allocate("ep-d");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->proxy_port, 5001U);
}

TEST(VpnRegistryTest, PortZeroNeverEntersTheFreePool) {
    // 0 is the "no proxy port" sentinel — a caller passing port_start = 0 must
    // not be able to hand it out as a real allocation.
    VpnRegistry reg("10.9.0.0/24", 0, 2);
    auto a = reg.allocate("ep-a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->proxy_port, 1U) << "pool must start at 1, not the 0 sentinel";
}

} // namespace
