/// Unit tests for the multi-tenant P1 core primitives (tenant_policy.{hpp,cpp}).
/// Emphasis on backward compatibility: the "default" tenant must reproduce the
/// pre-multi-tenant behaviour exactly.

#include <gtest/gtest.h>

#include "nlohmann/json.hpp"

#include "tenant_policy.hpp"
#include "psk_gen.hpp"

using namespace iot;

// ── valid_tenant_id ─────────────────────────────────────────────────────────

TEST(TenantId, AcceptsWellFormedSlugs) {
    EXPECT_TRUE(valid_tenant_id("acme"));
    EXPECT_TRUE(valid_tenant_id("default"));
    EXPECT_TRUE(valid_tenant_id("a"));
    EXPECT_TRUE(valid_tenant_id("acme-corp-2"));
    EXPECT_TRUE(valid_tenant_id(std::string(32, 'a')));   // max length
}

TEST(TenantId, RejectsMalformed) {
    EXPECT_FALSE(valid_tenant_id(""));
    EXPECT_FALSE(valid_tenant_id("*"));                   // platform operator
    EXPECT_FALSE(valid_tenant_id("Acme"));                // uppercase
    EXPECT_FALSE(valid_tenant_id("ac me"));               // space
    EXPECT_FALSE(valid_tenant_id("acme_corp"));           // underscore
    EXPECT_FALSE(valid_tenant_id("-acme"));               // leading dash
    EXPECT_FALSE(valid_tenant_id("acme-"));               // trailing dash
    EXPECT_FALSE(valid_tenant_id("acme:corp"));           // colon
    EXPECT_FALSE(valid_tenant_id(std::string(33, 'a')));  // too long
}

// ── split_endpoint / join_endpoint ──────────────────────────────────────────

TEST(SplitEndpoint, LegacyNoColonIsDefault) {
    EXPECT_EQ(split_endpoint("000000fe26a4ff"),
              (EndpointId{"default", "000000fe26a4ff"}));
}

TEST(SplitEndpoint, TenantQualified) {
    EXPECT_EQ(split_endpoint("acme:000000fe26a4ff"),
              (EndpointId{"acme", "000000fe26a4ff"}));
}

TEST(SplitEndpoint, BadPrefixFallsBackToDefaultWholeSerial) {
    // Invalid tenant slug -> whole string is a default-tenant serial.
    EXPECT_EQ(split_endpoint("Bad:serial"),
              (EndpointId{"default", "Bad:serial"}));
    // Empty serial after colon -> default + whole.
    EXPECT_EQ(split_endpoint("acme:"),
              (EndpointId{"default", "acme:"}));
}

TEST(JoinEndpoint, RoundTrips) {
    EXPECT_EQ(join_endpoint("default", "ser"), "ser");          // legacy on wire
    EXPECT_EQ(join_endpoint("", "ser"), "ser");
    EXPECT_EQ(join_endpoint("acme", "ser"), "acme:ser");
    // round-trip for a tenant-qualified ep
    auto e = split_endpoint("acme:ser");
    EXPECT_EQ(join_endpoint(e.tenant, e.serial), "acme:ser");
}

// ── bs_identity ─────────────────────────────────────────────────────────────

TEST(BsIdentity, DefaultMatchesLegacySha256) {
    const std::string serial = "000000fe26a4ff";
    // Legacy device + cloud compute sha256(endpoint)[:32]; default must match.
    EXPECT_EQ(bs_identity("default", serial), sha256_hex(serial).substr(0, 32));
    EXPECT_EQ(bs_identity("", serial), sha256_hex(serial).substr(0, 32));
}

TEST(BsIdentity, TenantQualifiedAndCollisionFree) {
    const std::string serial = "deadbeef";
    const std::string acme = bs_identity("acme", serial);
    const std::string globex = bs_identity("globex", serial);
    EXPECT_EQ(acme, sha256_hex("acme:" + serial).substr(0, 32));
    EXPECT_EQ(acme.size(), 32u);                 // 128-bit, fits tinydtls
    // Same serial, different tenants -> different identities (no collision).
    EXPECT_NE(acme, globex);
    EXPECT_NE(acme, bs_identity("default", serial));
}

// ── dm_identity / parse_dm_identity ─────────────────────────────────────────

TEST(DmIdentity, DefaultIsLegacyForm) {
    EXPECT_EQ(dm_identity("default", "abc"), "rpiabc@cloud.local");
    EXPECT_EQ(dm_identity("", "abc"), "rpiabc@cloud.local");
}

TEST(DmIdentity, TenantQualifiedForm) {
    EXPECT_EQ(dm_identity("acme", "abc"), "rpiabc@acme.cloud.local");
}

TEST(ParseDmIdentity, RoundTripsBothForms) {
    EXPECT_EQ(parse_dm_identity("rpiabc@cloud.local"),
              (EndpointId{"default", "abc"}));
    EXPECT_EQ(parse_dm_identity("rpiabc@acme.cloud.local"),
              (EndpointId{"acme", "abc"}));
    for (auto* id : {"rpi000ff@cloud.local", "rpi000ff@globex.cloud.local"}) {
        auto p = parse_dm_identity(id);
        EXPECT_EQ(dm_identity(p.tenant, p.serial), id);
    }
}

TEST(ParseDmIdentity, RejectsMalformed) {
    for (auto* bad : {"", "abc", "rpi@cloud.local", "xyzabc@cloud.local",
                      "rpiabc@", "rpiabc@.cloud.local", "rpiabc@A.cloud.local"}) {
        auto p = parse_dm_identity(bad);
        EXPECT_TRUE(p.tenant.empty() && p.serial.empty()) << "for: " << bad;
    }
}

// ── row_tenant / filter_rows_by_tenant ──────────────────────────────────────

TEST(RowTenant, DefaultsWhenAbsentOrEmpty) {
    EXPECT_EQ(row_tenant(R"({"serial":"s"})"), "default");
    EXPECT_EQ(row_tenant(R"({"serial":"s","tenant":""})"), "default");
    EXPECT_EQ(row_tenant(R"({"serial":"s","tenant":"acme"})"), "acme");
    EXPECT_EQ(row_tenant("not json"), "default");
}

TEST(FilterRows, ScopesByTenantWithLegacyAsDefault) {
    const std::string arr = R"([
        {"serial":"a","tenant":"acme"},
        {"serial":"b"},
        {"serial":"c","tenant":"globex"},
        {"serial":"d","tenant":"acme"}
    ])";
    auto acme = nlohmann::json::parse(filter_rows_by_tenant(arr, "acme"));
    ASSERT_EQ(acme.size(), 2u);
    EXPECT_EQ(acme[0]["serial"], "a");
    EXPECT_EQ(acme[1]["serial"], "d");

    // The untagged row falls into "default".
    auto def = nlohmann::json::parse(filter_rows_by_tenant(arr, "default"));
    ASSERT_EQ(def.size(), 1u);
    EXPECT_EQ(def[0]["serial"], "b");

    EXPECT_EQ(filter_rows_by_tenant(arr, "nobody"), "[]");
    EXPECT_EQ(filter_rows_by_tenant("garbage", "acme"), "[]");
}

// ── cloud.tenants registry ──────────────────────────────────────────────────

TEST(TenantRegistry, FindAndList) {
    const std::string reg = R"([
        {"id":"default","name":"Default","vpn.subnet":"10.9.0.0/24",
         "proxy.port.start":10000,"proxy.port.end":10050,
         "dm.uri":"coaps://h:5683","status":"active"},
        {"id":"acme","name":"Acme","vpn.subnet":"10.9.16.0/24",
         "proxy.port.start":11000,"proxy.port.end":11099,
         "dm.uri":"coaps://h:5683","status":"suspended"}
    ])";
    EXPECT_EQ(list_tenant_ids(reg),
              (std::vector<std::string>{"default", "acme"}));

    auto acme = find_tenant(reg, "acme");
    ASSERT_TRUE(acme.has_value());
    EXPECT_EQ(acme->vpn_subnet, "10.9.16.0/24");
    EXPECT_EQ(acme->proxy_start, 11000);
    EXPECT_EQ(acme->proxy_end, 11099);
    EXPECT_FALSE(acme->active);                 // status=suspended

    auto def = find_tenant(reg, "default");
    ASSERT_TRUE(def.has_value());
    EXPECT_TRUE(def->active);

    EXPECT_FALSE(find_tenant(reg, "ghost").has_value());
    EXPECT_FALSE(find_tenant("[]", "acme").has_value());
    EXPECT_TRUE(list_tenant_ids("garbage").empty());
}
