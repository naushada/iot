/// PSK provisioning (task B) — PSK generator tests.

#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>

#include "psk_gen.hpp"

TEST(PskGen, Generates64HexCharsFor32Bytes) {
    auto hex = iot::generate_psk_hex(32);
    EXPECT_EQ(64u, hex.size());
    for (char c : hex)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex char: " << c;
}

TEST(PskGen, HexDecodesTo32Bytes) {
    auto hex = iot::generate_psk_hex(32);
    auto bin = iot::hex_decode(hex);
    EXPECT_EQ(32u, bin.size());
}

TEST(PskGen, HexEncodeDecodeRoundTrip) {
    const unsigned char raw[] = {0x00, 0x01, 0xab, 0xff, 0x10, 0x7e};
    auto hex = iot::hex_encode(raw, sizeof raw);
    EXPECT_EQ("0001abff107e", hex);
    auto bin = iot::hex_decode(hex);
    ASSERT_EQ(sizeof raw, bin.size());
    for (std::size_t i = 0; i < sizeof raw; ++i)
        EXPECT_EQ(raw[i], static_cast<unsigned char>(bin[i]));
}

TEST(PskGen, HexDecodeRejectsBadInput) {
    EXPECT_EQ("", iot::hex_decode("abc"));    // odd length
    EXPECT_EQ("", iot::hex_decode("zz"));     // non-hex
    EXPECT_EQ("", iot::hex_decode("12g4"));   // non-hex
}

TEST(PskGen, NotConstantAcrossCalls) {
    // Two 32-byte draws colliding would be astronomically unlikely;
    // a few draws all-distinct is a sane entropy smoke test.
    std::set<std::string> seen;
    for (int i = 0; i < 5; ++i) seen.insert(iot::generate_psk_hex(32));
    EXPECT_EQ(5u, seen.size());
}

// ── HKDF (zero-touch BS PSK derivation) — apps/docs/tdd-bs-hkdf-zerotouch.md ──

TEST(Hkdf, Rfc5869TestCase1) {
    // RFC 5869 Appendix A.1 — SHA-256, with salt + info.
    std::string ikm  = iot::hex_decode(
        "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    std::string salt = iot::hex_decode("000102030405060708090a0b0c");
    std::string info = iot::hex_decode("f0f1f2f3f4f5f6f7f8f9");
    EXPECT_EQ("3cb25f25faacd57a90434f64d0362f2a"
              "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865",
              iot::hkdf_sha256(ikm, salt, info, 42));
}

TEST(Hkdf, Rfc5869TestCase3ZeroLengthSaltAndInfo) {
    // RFC 5869 Appendix A.3 — empty salt (→ HashLen zeros) and empty info.
    std::string ikm = iot::hex_decode(
        "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    EXPECT_EQ("8da4e775a563c18f715f802a063c5a31"
              "b8a11f5c5ee1879ec3454e5f3c738d2d"
              "9d201395faa4b61a96c8",
              iot::hkdf_sha256(ikm, /*salt=*/"", /*info=*/"", 42));
}

TEST(Hkdf, EmptySaltEqualsHashLenZeros) {
    // The "" salt path must equal an explicit 32-zero-byte salt.
    std::string ikm  = iot::hex_decode("0b0b0b0b0b0b0b0b");
    std::string info = "ctx";
    std::string zeros32(32, '\0');
    EXPECT_EQ(iot::hkdf_sha256(ikm, zeros32, info, 32),
              iot::hkdf_sha256(ikm, /*salt=*/"", info, 32));
}

TEST(DeriveBsPsk, MatchesSharedCrossCheckVector) {
    // This vector is shared VERBATIM with test_gen_bs_psk.py so the cloud C++
    // and the host Python flashing tool can never drift. Do not change one
    // side without the other — they MUST produce identical keys.
    const std::string master_hex =
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    EXPECT_EQ("223a82da7acb983c1372ec5e72c77d00"
              "8fc40281e737bb4aea689f53600d4fe5",
              iot::derive_bs_psk_hex(master_hex, "100000003d1f9c2e"));
}

TEST(DeriveBsPsk, IsDeterministicAndSerialBound) {
    const std::string m = iot::generate_psk_hex(32);
    EXPECT_EQ(iot::derive_bs_psk_hex(m, "serialA"),
              iot::derive_bs_psk_hex(m, "serialA"));   // deterministic
    EXPECT_NE(iot::derive_bs_psk_hex(m, "serialA"),
              iot::derive_bs_psk_hex(m, "serialB"));   // bound to serial
    EXPECT_EQ(64u, iot::derive_bs_psk_hex(m, "serialA").size());
}

TEST(DeriveBsPsk, EmptyOrBadMasterDisablesTier) {
    EXPECT_EQ("", iot::derive_bs_psk_hex("", "serial"));      // no master
    EXPECT_EQ("", iot::derive_bs_psk_hex("abc", "serial"));   // odd length
    EXPECT_EQ("", iot::derive_bs_psk_hex("zz", "serial"));    // non-hex
}

// ── base64 ───────────────────────────────────────────────────────────────────

TEST(Base64, RoundTripsIncludingNulAndEmpty) {
    EXPECT_EQ("", iot::base64_encode(""));
    EXPECT_EQ("", iot::base64_decode(""));
    EXPECT_EQ("aGVsbG8=", iot::base64_encode("hello"));   // known vector
    const std::string raw("\x00\x01\x02\xff\x10z", 6);
    EXPECT_EQ(raw, iot::base64_decode(iot::base64_encode(raw)));
}

TEST(Base64, RejectsMalformed) {
    EXPECT_EQ("", iot::base64_decode("abc"));     // length not multiple of 4
    EXPECT_EQ("", iot::base64_decode("a!=="));    // invalid char
    EXPECT_EQ("", iot::base64_decode("a=b="));    // misplaced padding
}

// ── BS master AES-256-GCM envelope ───────────────────────────────────────────

namespace {
// Deterministic vector — wrap_bs_master(KEK, MASTER, NONCE) produced this blob
// (verified in podman). Locks the on-disk envelope format so a future refactor
// that changes layout/AAD/base64 fails loudly.
const char* kEnvKek    = "0101010101010101010101010101010101010101010101010101010101010101";
const char* kEnvMaster = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const char* kEnvNonce  = "0b0b0b0b0b0b0b0b0b0b0b0b";
const char* kEnvBlob   =
    "CwsLCwsLCwsLCwsLvSPRai0N5R/JTXdybWG5SZ/lsc2/iF2o+2QR4pd/067Rwn3QtVkJvc/fXbVrVwBb";
} // namespace

TEST(BsMasterEnvelope, FixedVectorWrapsToKnownBlob) {
    EXPECT_EQ(kEnvBlob, iot::wrap_bs_master(kEnvKek, kEnvMaster, kEnvNonce));
}

TEST(BsMasterEnvelope, UnwrapRecoversMaster) {
    auto m = iot::unwrap_bs_master_hex(kEnvKek, kEnvBlob);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(kEnvMaster, *m);
    // …and the recovered master derives the same per-device PSK.
    EXPECT_EQ("223a82da7acb983c1372ec5e72c77d00"
              "8fc40281e737bb4aea689f53600d4fe5",
              iot::derive_bs_psk_hex(*m, "100000003d1f9c2e"));
}

TEST(BsMasterEnvelope, RoundTripWithRandomNonce) {
    const std::string kek = iot::generate_psk_hex(32);
    const std::string master = iot::generate_psk_hex(32);
    const std::string nonce = iot::generate_psk_hex(12);
    auto m = iot::unwrap_bs_master_hex(kek, iot::wrap_bs_master(kek, master, nonce));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(master, *m);
}

TEST(BsMasterEnvelope, FailsClosedOnTamperWrongKeyAndGarbage) {
    std::string bad = kEnvBlob;
    bad[5] = (bad[5] == 'A') ? 'B' : 'A';
    EXPECT_FALSE(iot::unwrap_bs_master_hex(kEnvKek, bad).has_value());   // tamper

    std::string kek2 = kEnvKek;
    kek2[0] = (kek2[0] == '0') ? '2' : '0';
    EXPECT_FALSE(iot::unwrap_bs_master_hex(kek2, kEnvBlob).has_value());  // wrong KEK

    EXPECT_FALSE(iot::unwrap_bs_master_hex("", kEnvBlob).has_value());    // no KEK
    EXPECT_FALSE(iot::unwrap_bs_master_hex(kEnvKek, "").has_value());     // empty
    EXPECT_FALSE(iot::unwrap_bs_master_hex(kEnvKek, "!!!!").has_value()); // bad b64
}

TEST(BsMasterEnvelope, WrapRejectsBadSizes) {
    EXPECT_EQ("", iot::wrap_bs_master("ab", kEnvMaster, kEnvNonce));      // KEK ≠ 32B
    EXPECT_EQ("", iot::wrap_bs_master(kEnvKek, kEnvMaster, "0b0b"));      // nonce ≠ 12B
    EXPECT_EQ("", iot::wrap_bs_master(kEnvKek, "", kEnvNonce));           // empty master
}
