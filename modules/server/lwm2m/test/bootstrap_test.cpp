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

// ── 3. Duplicate provision fails ──────────────────────────────────

TEST(BootstrapProvisionerTest, DuplicateProvisionFails) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg;
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    ASSERT_TRUE(provisioner.provision("urn:dev:gateway-3").has_value());
    auto result2 = provisioner.provision("urn:dev:gateway-3");
    EXPECT_FALSE(result2.has_value());
}

// ── 4. Provision fails when subnet exhausted ──────────────────────

TEST(BootstrapProvisionerTest, FailsWhenSubnetExhausted) {
    EndpointRegistry ep_reg;
    VpnRegistry vpn_reg("10.9.0.0/30", 5001, 5100);  // tiny subnet
    BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    ASSERT_TRUE(provisioner.provision("urn:dev:a").has_value());
    ASSERT_TRUE(provisioner.provision("urn:dev:b").has_value());
    auto result3 = provisioner.provision("urn:dev:c");
    EXPECT_FALSE(result3.has_value());
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
