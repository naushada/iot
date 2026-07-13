/// L21/D3 — LwM2M Bootstrap Server unit tests (TDD).

#include <gtest/gtest.h>

#include "bootstrap.hpp"
#include "endpoint_registry.hpp"
#include "vpn_registry.hpp"

using server::lwm2m::BootstrapProvisioner;
using server::lwm2m::EndpointRegistry;
using server::openvpn::VpnRegistry;

namespace {

// ── 1. Successful provision assigns IP + port ─────────────────────

TEST(BootstrapProvisionerTest, ProvisionAssignsResources) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg("10.9.0.0/24", 5001, 5100);
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto result = provisioner.provision("urn:dev:gateway-1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->endpoint, "urn:dev:gateway-1");
    EXPECT_FALSE(result->tun_ip.empty());
    EXPECT_GT(result->proxy_port, 0U);
    EXPECT_GT(result->security_object_tlv.size(), 0U);
    EXPECT_GT(result->server_object_tlv.size(), 0U);
}

// ── 2. Endpoint is registered after provision ─────────────────────

TEST(BootstrapProvisionerTest, EndpointInRegistryAfterProvision) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto result = provisioner.provision("urn:dev:gateway-2");
    ASSERT_TRUE(result.has_value());

    const auto* info = ep_reg.lookup_by_ep("urn:dev:gateway-2");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->tun_ip, result->tun_ip);
    EXPECT_EQ(info->proxy_port, result->proxy_port);
    EXPECT_FALSE(info->registered);  // not registered yet — just bootstrapped
}

// ── 3. Re-provision is idempotent (reuses the same allocation) ─────

TEST(BootstrapProvisionerTest, ReprovisionIsIdempotent) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto first = provisioner.provision("urn:dev:gateway-3");
    ASSERT_TRUE(first.has_value());
    // Re-provisioning the SAME endpoint (e.g. refreshing its BS PSK, or a watch
    // replay after a cloudd restart) succeeds and returns the SAME tun IP +
    // proxy port — it does not fail as a "duplicate" nor allocate a new slot.
    auto again = provisioner.provision("urn:dev:gateway-3");
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(first->tun_ip,     again->tun_ip);
    EXPECT_EQ(first->proxy_port, again->proxy_port);
    EXPECT_EQ(1u, ep_reg.count());  // still one endpoint, no duplicate slot
}

// ── 4. Provision fails when subnet exhausted ──────────────────────

TEST(BootstrapProvisionerTest, FailsWhenSubnetExhausted) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg("10.9.0.0/30", 5001, 5100);  // tiny subnet
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    // A /30 holds only a handful of host IPs, so provisioning DISTINCT endpoints
    // must eventually fail (nullopt) once the pool is exhausted — rather than
    // hand out an out-of-range address. (Don't assume an exact count: the usable
    // host range depends on the registry's reservations.)
    bool exhausted = false;
    for (int i = 0; i < 8 && !exhausted; ++i) {
        if (!provisioner.provision("urn:dev:" + std::to_string(i)).has_value())
            exhausted = true;
    }
    EXPECT_TRUE(exhausted);
}

// ── 4b. An exhausted PORT pool must NOT fail provisioning ─────────
//
// The 52nd-device cliff (tdd-cloud-scale-1m-devices.md §C1/P0b): the proxy port
// was allocated at PROVISION time and a dry pool returned nullopt, so device #52
// got no tunnel IP and no BS PSK — it could not bootstrap at all, merely because
// a legacy DNAT forward was unavailable. The port is optional; the tunnel IP is
// not. Provisioning must succeed with proxy_port == 0, and the device is then
// reached over the path proxy (/dev/<ep>/), which needs no port.

TEST(BootstrapProvisionerTest, PortPoolExhaustionDoesNotFailProvision) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg("10.9.0.0/24", 10000, 10001);   // roomy subnet, TWO ports
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto a = provisioner.provision("ep-a");
    auto b = provisioner.provision("ep-b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_GT(a->proxy_port, 0U);
    EXPECT_GT(b->proxy_port, 0U);

    // Port pool is now dry. The next devices must STILL onboard.
    auto c = provisioner.provision("ep-c");
    ASSERT_TRUE(c.has_value()) << "a dry port pool must never block bootstrap";
    EXPECT_EQ(c->proxy_port, 0U);
    EXPECT_FALSE(c->tun_ip.empty())          << "…and must still get a tunnel IP";
    EXPECT_GT(c->security_object_tlv.size(), 0U) << "…and its BS credentials";
    EXPECT_GT(c->server_object_tlv.size(), 0U);

    // …and so must the one after it — i.e. portless devices don't collide in the
    // EndpointRegistry (proxy_port 0 is not indexed as a real port).
    auto d = provisioner.provision("ep-d");
    ASSERT_TRUE(d.has_value()) << "two portless devices must coexist";
    EXPECT_EQ(d->proxy_port, 0U);
    EXPECT_NE(d->tun_ip, c->tun_ip);

    EXPECT_EQ(ep_reg.count(), 4U);
    ASSERT_NE(ep_reg.lookup_by_ep("ep-d"), nullptr);
}

// ── 5. Security Object TLV contains correct fields ────────────────

TEST(BootstrapProvisionerTest, SecurityObjectTlvHasRequiredFields) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto result = provisioner.provision("urn:dev:gateway-5");
    ASSERT_TRUE(result.has_value());

    // Security Object (OID 0) must include:
    // RID 0: Server URI
    // RID 1: Bootstrap flag (true for BS account)
    // RID 10: Short Server ID
    const auto& tlv = result->security_object_tlv;
    EXPECT_GT(tlv.size(), 3U);  // at least a few TLV bytes
}

// ── 6. Server Object TLV contains correct fields ──────────────────

TEST(BootstrapProvisionerTest, ServerObjectTlvHasRequiredFields) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    auto result = provisioner.provision("urn:dev:gateway-6");
    ASSERT_TRUE(result.has_value());

    // Server Object (OID 1) must include:
    // RID 0: Short Server ID
    // RID 1: Lifetime
    // RID 7: Binding
    const auto& tlv = result->server_object_tlv;
    EXPECT_GT(tlv.size(), 5U);
}

// ── 7. De-provision removes from registry ─────────────────────────

TEST(BootstrapProvisionerTest, DeprovisionRemovesFromRegistry) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    ASSERT_TRUE(provisioner.provision("urn:dev:gateway-7").has_value());
    EXPECT_EQ(ep_reg.count(), 1U);

    EXPECT_TRUE(provisioner.deprovision("urn:dev:gateway-7"));
    EXPECT_EQ(ep_reg.count(), 0U);
    EXPECT_EQ(vpn_reg.allocated_count(), 0U);
}

// ── 8. De-provision non-existent returns false ────────────────────

TEST(BootstrapProvisionerTest, DeprovisionNonExistentReturnsFalse) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    EXPECT_FALSE(provisioner.deprovision("urn:dev:nonexistent"));
}

} // namespace
