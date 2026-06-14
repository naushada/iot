/// L21/D1 — Endpoint Registry unit tests (TDD).
///
/// Tests the in-memory endpoint registry: add / remove / lookup by
/// endpoint name, tunnel IP, and proxy port.  No data-store dependency
/// — pure unit tests against the registry container directly.

#include <gtest/gtest.h>

#include "endpoint_registry.hpp"

#include <string>

using server::lwm2m::EndpointInfo;
using server::lwm2m::EndpointRegistry;

namespace {

// ── 1. Empty registry ───────────────────────────────────────────────

TEST(EndpointRegistryTest, EmptyRegistryReturnsNull) {
    EndpointRegistry reg;
    EXPECT_EQ(reg.lookup_by_ep("urn:dev:test"), nullptr);
    EXPECT_EQ(reg.lookup_by_tun_ip("10.9.0.2"), nullptr);
    EXPECT_EQ(reg.lookup_by_proxy_port(5001), nullptr);
    EXPECT_EQ(reg.count(), 0U);
}

// ── 2. Add + lookup by endpoint ─────────────────────────────────────

TEST(EndpointRegistryTest, AddAndLookupByEp) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep         = "urn:dev:gateway-1";
    info.tun_ip     = "10.9.0.10";
    info.proxy_port = 5001;
    info.registered = false;

    ASSERT_TRUE(reg.add(info));
    EXPECT_EQ(reg.count(), 1U);

    const auto* found = reg.lookup_by_ep("urn:dev:gateway-1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->tun_ip, "10.9.0.10");
    EXPECT_EQ(found->proxy_port, 5001);
}

// ── update_dev_tun_ip: record the openvpn-assigned address separately ───

TEST(EndpointRegistryTest, UpdateDevTunIpKeepsAllocationAndStoresActual) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep         = "urn:dev:gateway-1";
    info.tun_ip     = "10.9.0.10";   // registry pre-allocation
    info.proxy_port = 10000;
    ASSERT_TRUE(reg.add(info));

    // openvpn actually assigned .2 → record as dev_tun_ip; allocation stays.
    EXPECT_TRUE(reg.update_dev_tun_ip("urn:dev:gateway-1", "10.9.0.2"));
    const auto* e = reg.lookup_by_ep("urn:dev:gateway-1");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->tun_ip, "10.9.0.10");      // allocation preserved
    EXPECT_EQ(e->dev_tun_ip, "10.9.0.2");   // actual recorded
    // tun_ip index untouched (still resolves the allocation).
    ASSERT_NE(reg.lookup_by_tun_ip("10.9.0.10"), nullptr);

    // no-ops: unchanged value, and unknown endpoint.
    EXPECT_FALSE(reg.update_dev_tun_ip("urn:dev:gateway-1", "10.9.0.2"));
    EXPECT_FALSE(reg.update_dev_tun_ip("urn:dev:nope", "10.9.0.9"));
}

// ── 3. Duplicate endpoint rejected ──────────────────────────────────

TEST(EndpointRegistryTest, DuplicateEpRejected) {
    EndpointRegistry reg;
    EndpointInfo a;
    a.ep = "urn:dev:gateway-1"; a.tun_ip = "10.9.0.10"; a.proxy_port = 5001;
    ASSERT_TRUE(reg.add(a));

    EndpointInfo b;
    b.ep = "urn:dev:gateway-1"; b.tun_ip = "10.9.0.11"; b.proxy_port = 5002;
    EXPECT_FALSE(reg.add(b));  // duplicate endpoint name
    EXPECT_EQ(reg.count(), 1U);
}

// ── 4. Duplicate tunnel IP rejected ─────────────────────────────────

TEST(EndpointRegistryTest, DuplicateTunIpRejected) {
    EndpointRegistry reg;
    EndpointInfo a;
    a.ep = "urn:dev:gateway-1"; a.tun_ip = "10.9.0.10"; a.proxy_port = 5001;
    ASSERT_TRUE(reg.add(a));

    EndpointInfo b;
    b.ep = "urn:dev:gateway-2"; b.tun_ip = "10.9.0.10"; b.proxy_port = 5002;
    EXPECT_FALSE(reg.add(b));  // duplicate tunnel IP
    EXPECT_EQ(reg.count(), 1U);
}

// ── 5. Duplicate proxy port rejected ────────────────────────────────

TEST(EndpointRegistryTest, DuplicateProxyPortRejected) {
    EndpointRegistry reg;
    EndpointInfo a;
    a.ep = "urn:dev:gateway-1"; a.tun_ip = "10.9.0.10"; a.proxy_port = 5001;
    ASSERT_TRUE(reg.add(a));

    EndpointInfo b;
    b.ep = "urn:dev:gateway-2"; b.tun_ip = "10.9.0.11"; b.proxy_port = 5001;
    EXPECT_FALSE(reg.add(b));  // duplicate proxy port
    EXPECT_EQ(reg.count(), 1U);
}

// ── 6. Lookup by tunnel IP ──────────────────────────────────────────

TEST(EndpointRegistryTest, LookupByTunIp) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep = "urn:dev:gateway-1"; info.tun_ip = "10.9.0.10";
    info.proxy_port = 5001;
    reg.add(info);

    const auto* found = reg.lookup_by_tun_ip("10.9.0.10");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->ep, "urn:dev:gateway-1");
}

// ── 7. Lookup by proxy port ─────────────────────────────────────────

TEST(EndpointRegistryTest, LookupByProxyPort) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep = "urn:dev:gateway-1"; info.tun_ip = "10.9.0.10";
    info.proxy_port = 5001;
    reg.add(info);

    const auto* found = reg.lookup_by_proxy_port(5001);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->ep, "urn:dev:gateway-1");
}

// ── 8. Remove endpoint ──────────────────────────────────────────────

TEST(EndpointRegistryTest, RemoveEndpoint) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep = "urn:dev:gateway-1"; info.tun_ip = "10.9.0.10";
    info.proxy_port = 5001;
    reg.add(info);
    ASSERT_EQ(reg.count(), 1U);

    EXPECT_TRUE(reg.remove("urn:dev:gateway-1"));
    EXPECT_EQ(reg.count(), 0U);
    EXPECT_EQ(reg.lookup_by_ep("urn:dev:gateway-1"), nullptr);
    EXPECT_EQ(reg.lookup_by_tun_ip("10.9.0.10"), nullptr);
    EXPECT_EQ(reg.lookup_by_proxy_port(5001), nullptr);
}

// ── 9. Remove non-existent returns false ────────────────────────────

TEST(EndpointRegistryTest, RemoveNonExistentReturnsFalse) {
    EndpointRegistry reg;
    EXPECT_FALSE(reg.remove("urn:dev:nonexistent"));
}

// ── 10. Update state ────────────────────────────────────────────────

TEST(EndpointRegistryTest, UpdateState) {
    EndpointRegistry reg;
    EndpointInfo info;
    info.ep = "urn:dev:gateway-1"; info.tun_ip = "10.9.0.10";
    info.proxy_port = 5001; info.registered = false;
    reg.add(info);

    // Update registration status
    EXPECT_TRUE(reg.update_state("urn:dev:gateway-1", true));
    const auto* found = reg.lookup_by_ep("urn:dev:gateway-1");
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->registered);

    // Update to false
    EXPECT_TRUE(reg.update_state("urn:dev:gateway-1", false));
    EXPECT_FALSE(reg.lookup_by_ep("urn:dev:gateway-1")->registered);
}

// ── 11. Update non-existent ─────────────────────────────────────────

TEST(EndpointRegistryTest, UpdateStateNonExistentReturnsFalse) {
    EndpointRegistry reg;
    EXPECT_FALSE(reg.update_state("urn:dev:nonexistent", true));
}

// ── 12. List all endpoints ──────────────────────────────────────────

TEST(EndpointRegistryTest, ListAll) {
    EndpointRegistry reg;
    EndpointInfo a{"urn:dev:a", "10.9.0.10", 5001, false};
    EndpointInfo b{"urn:dev:b", "10.9.0.11", 5002, true};
    reg.add(a); reg.add(b);

    auto all = reg.list_all();
    EXPECT_EQ(all.size(), 2U);
    // Order doesn't matter — check both are present
    bool found_a = false, found_b = false;
    for (const auto& e : all) {
        if (e.ep == "urn:dev:a") found_a = true;
        if (e.ep == "urn:dev:b") found_b = true;
    }
    EXPECT_TRUE(found_a); EXPECT_TRUE(found_b);
}

} // namespace
