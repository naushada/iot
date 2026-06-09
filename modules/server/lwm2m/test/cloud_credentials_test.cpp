/// PSK provisioning (task M) — cloud credential helper tests.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cloud_credentials.hpp"

using server::lwm2m::format_identity;
using server::lwm2m::upsert_credential;
using server::lwm2m::remove_credential;
using server::lwm2m::credentials_for_instance;
using server::lwm2m::upsert_vpn_cert;
using server::lwm2m::vpn_cert_for;
using nlohmann::json;

TEST(CloudCredentials, FormatsIdentity) {
    EXPECT_EQ("rpi100000abcd@cloud.local", format_identity("100000abcd"));
}

TEST(CloudCredentials, UpsertAppendsNewRecord) {
    auto out = upsert_credential("[]", "SER1", "bbbb", "dddd");
    auto arr = json::parse(out);
    ASSERT_EQ(1u, arr.size());
    EXPECT_EQ("SER1", arr[0]["serial"]);
    EXPECT_EQ("rpiSER1@cloud.local", arr[0]["identity"]);
    EXPECT_EQ("rpiSER1@cloud.local", arr[0]["dm.psk.id"]);
    EXPECT_EQ("bbbb", arr[0]["bs.psk.key"]);
    EXPECT_EQ("dddd", arr[0]["dm.psk.key"]);
}

TEST(CloudCredentials, UpsertIsIdempotentOnSerial) {
    auto out = upsert_credential("[]", "SER1", "bbbb", "dddd");
    out = upsert_credential(out, "SER2", "eeee", "ffff");
    // Re-provision SER1 with a new BS key → replace, not duplicate.
    out = upsert_credential(out, "SER1", "9999", "8888");
    auto arr = json::parse(out);
    ASSERT_EQ(2u, arr.size());
    int ser1 = 0; std::string bs1;
    for (auto& e : arr) if (e["serial"] == "SER1") { ser1++; bs1 = e["bs.psk.key"]; }
    EXPECT_EQ(1, ser1);
    EXPECT_EQ("9999", bs1);
}

TEST(CloudCredentials, RemoveDropsEntry) {
    auto out = upsert_credential("[]", "SER1", "bbbb", "dddd");
    out = upsert_credential(out, "SER2", "eeee", "ffff");
    out = remove_credential(out, "SER1");
    auto arr = json::parse(out);
    ASSERT_EQ(1u, arr.size());
    EXPECT_EQ("SER2", arr[0]["serial"]);
}

TEST(CloudCredentials, RemoveAbsentIsNoop) {
    auto out = upsert_credential("[]", "SER1", "bbbb", "dddd");
    out = remove_credential(out, "NOPE");
    EXPECT_EQ(1u, json::parse(out).size());
}

TEST(CloudCredentials, EmptyStringTreatedAsEmptyArray) {
    auto out = upsert_credential("", "SER1", "bbbb", "dddd");
    EXPECT_EQ(1u, json::parse(out).size());
}

TEST(CloudCredentials, RejectsNonArray) {
    EXPECT_THROW(upsert_credential("{}", "S", "b", "d"), std::runtime_error);
}

TEST(CloudCredentials, BsInstanceKeyedByRawSerial) {
    auto arr = upsert_credential("[]", "SER1", "bbbb", "dddd");
    arr = upsert_credential(arr, "SER2", "eeee", "ffff");
    auto pairs = credentials_for_instance(arr, /*is_bs*/true);
    ASSERT_EQ(2u, pairs.size());
    // BS identity is the RAW serial (what the device sends on the wire).
    EXPECT_EQ("SER1", pairs[0].identity);
    EXPECT_EQ("bbbb", pairs[0].key_hex);
    EXPECT_EQ("SER2", pairs[1].identity);
    EXPECT_EQ("eeee", pairs[1].key_hex);
}

TEST(CloudCredentials, DmInstanceKeyedByFormattedIdentity) {
    auto arr = upsert_credential("[]", "SER1", "bbbb", "dddd");
    auto pairs = credentials_for_instance(arr, /*is_bs*/false);
    ASSERT_EQ(1u, pairs.size());
    EXPECT_EQ("rpiSER1@cloud.local", pairs[0].identity);
    EXPECT_EQ("dddd", pairs[0].key_hex);   // DM key, not BS key
}

TEST(CloudCredentials, InstanceSkipsRecordsMissingKey) {
    // A record with no bs.psk.key is skipped for the BS instance.
    json arr = json::array();
    arr.push_back({{"serial","S1"},{"identity","rpiS1@cloud.local"},
                   {"dm.psk.id","rpiS1@cloud.local"},{"dm.psk.key","dd"}});
    auto bs = credentials_for_instance(arr.dump(), true);
    EXPECT_TRUE(bs.empty());
    auto dm = credentials_for_instance(arr.dump(), false);
    EXPECT_EQ(1u, dm.size());
}

/* ─────────────────────── VPN cert family (Phase 2/3) ──────────────────── */

TEST(CloudCredentials, UpsertVpnCertMergesIntoExistingRecord) {
    auto arr = upsert_credential("[]", "SER1", "bbbb", "dddd");   // PSK record first
    arr = upsert_vpn_cert(arr, "SER1", "CA-PEM", "CERT-PEM", "KEY-PEM");
    auto fam = vpn_cert_for(arr, "SER1");
    ASSERT_TRUE(fam.has_value());
    EXPECT_EQ("CA-PEM",   fam->ca);
    EXPECT_EQ("CERT-PEM", fam->cert);
    EXPECT_EQ("KEY-PEM",  fam->key);
    // PSK fields survive the merge.
    auto pairs = credentials_for_instance(arr, /*is_bs*/true);
    ASSERT_EQ(1u, pairs.size());
    EXPECT_EQ("bbbb", pairs[0].key_hex);
}

TEST(CloudCredentials, UpsertVpnCertCreatesRecordWhenAbsent) {
    auto arr = upsert_vpn_cert("[]", "SER9", "CA", "CRT", "KEY");
    auto fam = vpn_cert_for(arr, "SER9");
    ASSERT_TRUE(fam.has_value());
    EXPECT_EQ("CRT", fam->cert);
}

TEST(CloudCredentials, VpnCertForNulloptWhenIncompleteOrMissing) {
    EXPECT_FALSE(vpn_cert_for("[]", "NOPE").has_value());
    auto arr = upsert_credential("[]", "SER1", "bbbb", "dddd");   // PSK only
    EXPECT_FALSE(vpn_cert_for(arr, "SER1").has_value());
}
