/// PSK provisioning (tasks E/K/G) — pure policy tests.

#include <gtest/gtest.h>

#include "provisioning_policy.hpp"
#include "psk_gen.hpp"

using iot::resolve_endpoint;

TEST(EndpointResolution, CliOverrideWins) {
    auto r = resolve_endpoint("urn:dev:cli", std::string("SER123"),
                              /*is_rpi*/true, "DETECT");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("urn:dev:cli", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);   // never auto-writes on override
}

TEST(EndpointResolution, DataStoreSerialUsedWhenNoCli) {
    auto r = resolve_endpoint("", std::string("SER123"), true, "DETECT");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("SER123", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);   // already persisted
}

TEST(EndpointResolution, RpiAutoFillWhenSerialEmpty) {
    auto r = resolve_endpoint("", std::nullopt, /*is_rpi*/true, "100000abcd");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("100000abcd", r.endpoint);
    EXPECT_EQ("100000abcd", r.serial_to_write);  // signal caller to persist
}

TEST(EndpointResolution, RpiAutoFillWhenSerialBlank) {
    // ds_serial present but empty string behaves like "not set".
    auto r = resolve_endpoint("", std::string(""), true, "100000abcd");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("100000abcd", r.serial_to_write);
}

TEST(EndpointResolution, NonRpiNoSerialDefers) {
    auto r = resolve_endpoint("", std::nullopt, /*is_rpi*/false, "");
    EXPECT_FALSE(r.ready);
    EXPECT_EQ("", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);
}

TEST(EndpointResolution, NonRpiInstallerSerialUsed) {
    auto r = resolve_endpoint("", std::string("INSTALLER-SN"), false, "");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("INSTALLER-SN", r.endpoint);
}

TEST(RestartOnPskChange, NoRestartBeforeInit) {
    EXPECT_FALSE(iot::should_restart_on_psk_change(
        /*initialized*/false, "old", "new"));
}

TEST(RestartOnPskChange, NoRestartOnSelfWrite) {
    // loaded == observed → the value we just wrote; no restart.
    EXPECT_FALSE(iot::should_restart_on_psk_change(true, "abcd", "abcd"));
}

TEST(RestartOnPskChange, RestartOnGenuineChange) {
    EXPECT_TRUE(iot::should_restart_on_psk_change(true, "abcd", "ef01"));
}

TEST(CoapClientError, ClassifiesByUpperBits) {
    EXPECT_TRUE(iot::is_coap_client_error(0x83));   // 4.03 Forbidden
    EXPECT_TRUE(iot::is_coap_client_error(0x80));   // 4.00 Bad Request
    EXPECT_FALSE(iot::is_coap_client_error(0x41));  // 2.01 Created
    EXPECT_FALSE(iot::is_coap_client_error(0xA0));  // 5.00 Server Error
}

TEST(Rebootstrap, TriggersOnDtlsFailure) {
    EXPECT_TRUE(iot::should_rebootstrap(/*dtls*/true, /*reg*/false));
}

TEST(Rebootstrap, TriggersOnRegistrationReject) {
    EXPECT_TRUE(iot::should_rebootstrap(false, true));
}

TEST(Rebootstrap, NoTriggerWhenHealthy) {
    EXPECT_FALSE(iot::should_rebootstrap(false, false));
}

// ── Zero-touch BS PSK resolver — tdd-bs-hkdf-zerotouch.md ─────────────────────

namespace {
// Shared with apps/test/psk_gen_test.cpp / test_gen_bs_psk.py.
const char* kMaster = "000102030405060708090a0b0c0d0e0f"
                      "101112131415161718191a1b1c1d1e1f";
const char* kSerial = "100000003d1f9c2e";
const char* kDerivedPsk = "223a82da7acb983c1372ec5e72c77d00"
                          "8fc40281e737bb4aea689f53600d4fe5";
// sha256("100000003d1f9c2e")[:32] — the commissioned-tier wire identity.
const char* kSha256Id = "c5dd8a100c796a384fdaec5334b0da71";
} // namespace

TEST(ResolveBsPsk, CommissionedRowWinsOverDerivation) {
    // A row keyed by sha256(serial)[:32] returns its stored key even when a
    // master is set — the device-ui-provisioned tier is unchanged.
    const std::string creds =
        R"([{"serial":"100000003d1f9c2e","bs.psk.key":"feedface"}])";
    EXPECT_EQ("feedface", iot::resolve_bs_psk(creds, kSha256Id, kMaster));
}

TEST(ResolveBsPsk, FallsBackToFormattedIdentity) {
    // A device with iot.bs.psk.override=true presents its DM-style identity at
    // the BS handshake. With no sha256 match, fall back to the formatted
    // identity / dm.psk.id and return the same bs.psk.key. No master needed.
    const std::string creds =
        R"([{"serial":"100000003d1f9c2e",)"
        R"("identity":"rpi100000003d1f9c2e@cloud.local",)"
        R"("dm.psk.id":"rpi100000003d1f9c2e@cloud.local",)"
        R"("bs.psk.key":"feedface"}])";
    EXPECT_EQ("feedface",
              iot::resolve_bs_psk(creds, "rpi100000003d1f9c2e@cloud.local", ""));
    // The canonical sha256 path still resolves the same row.
    EXPECT_EQ("feedface", iot::resolve_bs_psk(creds, kSha256Id, ""));
}

TEST(ResolveBsPsk, FormattedIdentityNoMatchYieldsEmpty) {
    // A formatted identity for an unprovisioned serial, no master → no key.
    const std::string creds =
        R"([{"serial":"100000003d1f9c2e",)"
        R"("identity":"rpi100000003d1f9c2e@cloud.local",)"
        R"("bs.psk.key":"feedface"}])";
    EXPECT_EQ("", iot::resolve_bs_psk(creds, "rpiDEADBEEF@cloud.local", ""));
}

TEST(ResolveBsPsk, DerivesFromRawSerialWhenNoRow) {
    // Zero-touch: no row, master set, peer presented its raw serial verbatim.
    EXPECT_EQ(kDerivedPsk, iot::resolve_bs_psk("[]", kSerial, kMaster));
}

TEST(ResolveBsPsk, NoMasterAndNoRowYieldsEmpty) {
    EXPECT_EQ("", iot::resolve_bs_psk("[]", kSerial, ""));
}

TEST(ResolveBsPsk, EmptyPresentedYieldsEmpty) {
    EXPECT_EQ("", iot::resolve_bs_psk("[]", "", kMaster));
}

TEST(ResolveBsPsk, MalformedCredentialsFallsThroughToDerive) {
    // Garbage credentials must not crash; with a master we still derive.
    EXPECT_EQ(kDerivedPsk, iot::resolve_bs_psk("not json", kSerial, kMaster));
    EXPECT_EQ("", iot::resolve_bs_psk("not json", kSerial, ""));
}

TEST(ShouldMintDm, MintsWhenNoRow) {
    EXPECT_TRUE(iot::should_mint_dm("[]", kSerial));
    EXPECT_TRUE(iot::should_mint_dm(
        R"([{"serial":"other"}])", kSerial));
}

TEST(ShouldMintDm, ReusesWhenRowExists) {
    EXPECT_FALSE(iot::should_mint_dm(
        R"([{"serial":"100000003d1f9c2e","dm.psk.key":"x"}])", kSerial));
}

TEST(ShouldMintDm, EmptySerialOrBadJsonNeverMints) {
    EXPECT_FALSE(iot::should_mint_dm("[]", ""));
    EXPECT_TRUE(iot::should_mint_dm("not json", kSerial));  // no row → mint
}

// ── DM identity + zero-touch DM resolver ─────────────────────────────────────

namespace {
// derive_dm_psk_hex(kMaster, kSerial), computed independently.
const char* kDerivedDm = "f9123ef9d4cc65bca1586fe27faff3e7"
                         "0adde77d266b30fc3645f49466f5113a";
} // namespace

TEST(DmIdentity, FormatAndParseRoundTrip) {
    EXPECT_EQ("rpi100000003d1f9c2e@cloud.local",
              iot::format_dm_identity(kSerial));
    EXPECT_EQ(kSerial,
              iot::serial_from_dm_identity(iot::format_dm_identity(kSerial)));
}

TEST(DmIdentity, ParseRejectsNonMatching) {
    EXPECT_EQ("", iot::serial_from_dm_identity("rpi@cloud.local"));     // no serial
    EXPECT_EQ("", iot::serial_from_dm_identity("100000003d1f9c2e"));    // raw serial
    EXPECT_EQ("", iot::serial_from_dm_identity("rpiX@example.com"));    // wrong suffix
    EXPECT_EQ("", iot::serial_from_dm_identity(""));
}

TEST(DeriveDmPsk, DiffersFromBsAndIsDeterministic) {
    EXPECT_EQ(kDerivedDm, iot::derive_dm_psk_hex(kMaster, kSerial));
    EXPECT_NE(iot::derive_bs_psk_hex(kMaster, kSerial),
              iot::derive_dm_psk_hex(kMaster, kSerial));   // domain-separated
    EXPECT_EQ("", iot::derive_dm_psk_hex("", kSerial));    // no master
}

TEST(ResolveDmPsk, CommissionedRowWinsOverDerivation) {
    const std::string creds =
        R"([{"dm.psk.id":"rpi100000003d1f9c2e@cloud.local","dm.psk.key":"cafe"}])";
    EXPECT_EQ("cafe", iot::resolve_dm_psk(
        creds, "rpi100000003d1f9c2e@cloud.local", kMaster));
}

TEST(ResolveDmPsk, DerivesWhenNoRow) {
    EXPECT_EQ(kDerivedDm, iot::resolve_dm_psk(
        "[]", iot::format_dm_identity(kSerial), kMaster));
}

TEST(ResolveDmPsk, NoMasterOrUnparseableYieldsEmpty) {
    EXPECT_EQ("", iot::resolve_dm_psk("[]", iot::format_dm_identity(kSerial), ""));
    EXPECT_EQ("", iot::resolve_dm_psk("[]", "not-a-dm-id", kMaster));
    EXPECT_EQ("", iot::resolve_dm_psk("[]", "", kMaster));
}
