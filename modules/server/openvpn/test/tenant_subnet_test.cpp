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
