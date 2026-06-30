/// Unit tests for the multi-tenant VPN subnet math + nft isolation rules
/// (tenant_subnet.{hpp,cpp}). Pure logic — no tun/openvpn/nft needed.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "tenant_subnet.hpp"

using namespace server::openvpn;

// ── subnets_overlap ─────────────────────────────────────────────────────────

TEST(SubnetsOverlap, DisjointAndOverlapping) {
    EXPECT_FALSE(subnets_overlap("10.9.16.0/24", "10.9.17.0/24"));
    EXPECT_TRUE(subnets_overlap("10.9.16.0/24", "10.9.16.128/25"));   // contained
    EXPECT_TRUE(subnets_overlap("10.9.0.0/16", "10.9.16.0/24"));      // pool ⊃ block
    EXPECT_TRUE(subnets_overlap("10.9.16.0/24", "10.9.16.0/24"));     // identical
    EXPECT_FALSE(subnets_overlap("10.9.16.0/24", "garbage"));         // malformed
}

// ── allocate_tenant_subnet ──────────────────────────────────────────────────

TEST(AllocTenantSubnet, FirstFreeBlockSkipsUsed) {
    // Empty pool → first /24 is the pool base.
    EXPECT_EQ(allocate_tenant_subnet("10.9.0.0/16", {}), "10.9.0.0/24");
    // Skips used blocks, returns the next free one.
    EXPECT_EQ(allocate_tenant_subnet("10.9.0.0/16",
                                     {"10.9.0.0/24", "10.9.1.0/24"}),
              "10.9.2.0/24");
    // Skips a non-contiguous used block.
    EXPECT_EQ(allocate_tenant_subnet("10.9.0.0/16", {"10.9.0.0/24"}),
              "10.9.1.0/24");
}

TEST(AllocTenantSubnet, NonOverlapWithExisting) {
    auto s = allocate_tenant_subnet("10.9.0.0/16",
                                    {"10.9.0.0/24", "10.9.2.0/24"});
    EXPECT_EQ(s, "10.9.1.0/24");
    EXPECT_FALSE(subnets_overlap(s, "10.9.0.0/24"));
    EXPECT_FALSE(subnets_overlap(s, "10.9.2.0/24"));
}

TEST(AllocTenantSubnet, ExhaustionAndBadInput) {
    // A /24 pool yields exactly one /24 block; once used, exhausted.
    EXPECT_EQ(allocate_tenant_subnet("10.9.5.0/24", {}), "10.9.5.0/24");
    EXPECT_EQ(allocate_tenant_subnet("10.9.5.0/24", {"10.9.5.0/24"}), "");
    // tenant_prefix smaller than the pool prefix is invalid.
    EXPECT_EQ(allocate_tenant_subnet("10.9.0.0/24", {}, /*prefix*/16), "");
    EXPECT_EQ(allocate_tenant_subnet("garbage", {}), "");
}

// ── build_tenant_isolation_rules ────────────────────────────────────────────

TEST(IsolationRules, PairwiseCrossTenantDrops) {
    auto r = build_tenant_isolation_rules({"10.9.16.0/24", "10.9.17.0/24"});
    EXPECT_NE(r.find("table ip iot_tenant_isol"), std::string::npos);
    // both cross directions dropped
    EXPECT_NE(r.find("ip saddr 10.9.16.0/24 ip daddr 10.9.17.0/24 drop"),
              std::string::npos);
    EXPECT_NE(r.find("ip saddr 10.9.17.0/24 ip daddr 10.9.16.0/24 drop"),
              std::string::npos);
    // no same-tenant self-drop
    EXPECT_EQ(r.find("ip saddr 10.9.16.0/24 ip daddr 10.9.16.0/24"),
              std::string::npos);
}

TEST(IsolationRules, SingleOrEmptyTenantHasNoDrops) {
    EXPECT_EQ(build_tenant_isolation_rules({}).find(" drop"),
              std::string::npos);
    EXPECT_EQ(build_tenant_isolation_rules({"10.9.16.0/24"}).find(" drop"),
              std::string::npos);
}

TEST(IsolationRules, ThreeTenantsSixDirectedDrops) {
    auto r = build_tenant_isolation_rules(
        {"10.9.16.0/24", "10.9.17.0/24", "10.9.18.0/24"});
    std::size_t n = 0, pos = 0;
    while ((pos = r.find(" drop\n", pos)) != std::string::npos) { ++n; pos += 6; }
    EXPECT_EQ(n, 6U);   // 3 tenants → 3*2 directed pairs
}

// ── assign_missing_subnets ──────────────────────────────────────────────────

TEST(AssignMissingSubnets, FillsOnlyMissingNonOverlapping) {
    const std::string in = R"([
        {"id":"acme"},
        {"id":"globex","vpn.subnet":"10.9.16.0/24"},
        {"id":"initech"}
    ])";
    auto [out, changed] = assign_missing_subnets(in, "10.9.16.0/20");
    EXPECT_TRUE(changed);
    auto j = nlohmann::json::parse(out);
    // globex keeps its pre-assigned subnet
    EXPECT_EQ(j[1]["vpn.subnet"], "10.9.16.0/24");
    // acme + initech get fresh, distinct, non-overlapping /24s (not globex's)
    const std::string a = j[0]["vpn.subnet"], c = j[2]["vpn.subnet"];
    EXPECT_FALSE(a.empty());
    EXPECT_FALSE(c.empty());
    EXPECT_NE(a, c);
    EXPECT_FALSE(subnets_overlap(a, "10.9.16.0/24"));
    EXPECT_FALSE(subnets_overlap(c, "10.9.16.0/24"));
    EXPECT_FALSE(subnets_overlap(a, c));
}

TEST(AssignMissingSubnets, Idempotent) {
    const std::string in = R"([{"id":"acme"}])";
    auto [out1, ch1] = assign_missing_subnets(in, "10.9.16.0/20");
    EXPECT_TRUE(ch1);
    auto [out2, ch2] = assign_missing_subnets(out1, "10.9.16.0/20");
    EXPECT_FALSE(ch2);                 // nothing left to assign
    EXPECT_EQ(nlohmann::json::parse(out1), nlohmann::json::parse(out2));
}

TEST(AssignMissingSubnets, BadInputUnchanged) {
    auto [out, changed] = assign_missing_subnets("not json", "10.9.16.0/20");
    EXPECT_FALSE(changed);
    EXPECT_EQ(out, "not json");
}

// ── tenant_at_capacity ──────────────────────────────────────────────────────

TEST(TenantAtCapacity, EnforcesMaxDevices) {
    const std::string tenants = R"([{"id":"acme","max.devices":2}])";
    const std::string two = R"([
        {"serial":"a","tenant":"acme"},
        {"serial":"b","tenant":"acme"}
    ])";
    // full → a NEW serial is blocked
    EXPECT_TRUE(tenant_at_capacity(tenants, two, "acme", "c"));
    // full → re-provisioning an EXISTING serial is allowed
    EXPECT_FALSE(tenant_at_capacity(tenants, two, "acme", "a"));
    // under cap → allowed
    const std::string one = R"([{"serial":"a","tenant":"acme"}])";
    EXPECT_FALSE(tenant_at_capacity(tenants, one, "acme", "c"));
}

TEST(TenantAtCapacity, UnlimitedWhenNoMax) {
    const std::string tenants = R"([{"id":"acme"}])";          // no max.devices
    const std::string many = R"([
        {"serial":"a","tenant":"acme"},{"serial":"b","tenant":"acme"}
    ])";
    EXPECT_FALSE(tenant_at_capacity(tenants, many, "acme", "c"));
    // Unknown tenant → no cap.
    EXPECT_FALSE(tenant_at_capacity("[]", many, "acme", "c"));
}

TEST(TenantAtCapacity, CountsDefaultTenantUntagged) {
    const std::string tenants = R"([{"id":"default","max.devices":1}])";
    const std::string one = R"([{"serial":"a"}])";            // untagged == default
    EXPECT_TRUE(tenant_at_capacity(tenants, one, "default", "b"));
    EXPECT_FALSE(tenant_at_capacity(tenants, one, "default", "a"));
}

// ── allocate_ip_in_subnet ───────────────────────────────────────────────────

TEST(AllocateIpInSubnet, FirstHostIsNetworkPlus2) {
    // .0 network, .1 gateway → first device host is .2
    EXPECT_EQ(allocate_ip_in_subnet("10.9.16.0/24", {}), "10.9.16.2");
}

TEST(AllocateIpInSubnet, SkipsUsed) {
    EXPECT_EQ(allocate_ip_in_subnet("10.9.16.0/24",
              {"10.9.16.2", "10.9.16.3"}), "10.9.16.4");
}

TEST(AllocateIpInSubnet, ExhaustionAndBadInput) {
    // /30 = .0 net, .1 gw, .2 host, .3 bcast → exactly one assignable (.2)
    EXPECT_EQ(allocate_ip_in_subnet("10.9.16.0/30", {}), "10.9.16.2");
    EXPECT_EQ(allocate_ip_in_subnet("10.9.16.0/30", {"10.9.16.2"}), "");
    EXPECT_EQ(allocate_ip_in_subnet("10.9.16.0/31", {}), "");   // no room
    EXPECT_EQ(allocate_ip_in_subnet("garbage", {}), "");
}

// ── build_ccd_entry ─────────────────────────────────────────────────────────

TEST(BuildCcdEntry, UsesServerMaskNotTenantMask) {
    // server pool /16 → netmask 255.255.0.0 even though the IP is in a /24
    EXPECT_EQ(build_ccd_entry("10.9.16.2", "10.9.0.0/16"),
              "ifconfig-push 10.9.16.2 255.255.0.0\n");
    EXPECT_EQ(build_ccd_entry("10.9.0.5", "10.9.0.0/24"),
              "ifconfig-push 10.9.0.5 255.255.255.0\n");
}

TEST(BuildCcdEntry, BadInput) {
    EXPECT_EQ(build_ccd_entry("not-an-ip", "10.9.0.0/16"), "");
    EXPECT_EQ(build_ccd_entry("10.9.16.2", "garbage"), "");
}

// ── plan_ccd_files ──────────────────────────────────────────────────────────

TEST(PlanCcdFiles, OnlyTenantRowsWithTunIp) {
    // default/untagged rows and rows without a tun_ip get NO CCD file; only
    // tenant-tagged rows with a tun_ip are pinned, using the server pool mask.
    const std::string eps = R"([
        {"endpoint":"ser-acme","tenant":"acme","tun_ip":"10.9.16.2"},
        {"endpoint":"ser-def","tun_ip":"10.9.0.5"},
        {"endpoint":"ser-glx","tenant":"globex","tun_ip":"10.9.17.2"},
        {"endpoint":"ser-noip","tenant":"acme"},
        {"endpoint":"ser-defx","tenant":"default","tun_ip":"10.9.0.9"}
    ])";
    auto plan = plan_ccd_files(eps, "10.9.0.0/16");
    ASSERT_EQ(plan.size(), 2u);                       // acme + globex only
    EXPECT_EQ(plan[0].serial, "ser-acme");
    EXPECT_EQ(plan[0].contents, "ifconfig-push 10.9.16.2 255.255.0.0\n");
    EXPECT_EQ(plan[1].serial, "ser-glx");
    EXPECT_EQ(plan[1].contents, "ifconfig-push 10.9.17.2 255.255.0.0\n");
}

TEST(PlanCcdFiles, BadInputs) {
    EXPECT_TRUE(plan_ccd_files("not json", "10.9.0.0/16").empty());
    EXPECT_TRUE(plan_ccd_files("{}", "10.9.0.0/16").empty());          // not array
    EXPECT_TRUE(plan_ccd_files(
        R"([{"endpoint":"a","tenant":"acme","tun_ip":"10.9.16.2"}])",
        "garbage").empty());                                          // bad pool
}
