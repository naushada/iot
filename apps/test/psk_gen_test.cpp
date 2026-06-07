/// PSK provisioning (task B) — PSK generator tests.

#include <gtest/gtest.h>

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
