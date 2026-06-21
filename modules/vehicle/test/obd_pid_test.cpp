#include "obd_pid.hpp"

#include <gtest/gtest.h>

using namespace vehicle::obd;

// ── Request framing ────────────────────────────────────────────────
TEST(ObdRequest, Mode01SinglePidFrame) {
    auto f = build_request(kPidRpm);
    EXPECT_EQ(0x02, f[0]);                 // single frame, 2 data bytes
    EXPECT_EQ(kModeCurrentData, f[1]);     // 0x01
    EXPECT_EQ(kPidRpm, f[2]);              // 0x0C
    for (std::size_t i = 3; i < f.size(); ++i) EXPECT_EQ(kPad, f[i]);
}

TEST(ObdRequest, HonoursExplicitMode) {
    auto f = build_request(0x00, kModeDtc);
    EXPECT_EQ(kModeDtc, f[1]);
}

// ── Response decoding (golden frames) ──────────────────────────────
TEST(ObdDecode, Rpm) {
    // A=0x0F, B=0xA0 → (256*15 + 160)/4 = 1000 rpm.
    const std::uint8_t d[] = {0x04, 0x41, 0x0C, 0x0F, 0xA0};
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_EQ(kPidRpm, v.pid);
    EXPECT_DOUBLE_EQ(1000.0, v.value);
    EXPECT_STREQ("rpm", v.unit);
    EXPECT_EQ("1000", to_ds_string(v));
}

TEST(ObdDecode, Speed) {
    const std::uint8_t d[] = {0x03, 0x41, 0x0D, 0x50};  // 0x50 = 80 km/h
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_DOUBLE_EQ(80.0, v.value);
    EXPECT_STREQ("km/h", v.unit);
    EXPECT_EQ("80", to_ds_string(v));
}

TEST(ObdDecode, CoolantTempOffset) {
    const std::uint8_t d[] = {0x03, 0x41, 0x05, 0x5A};  // 90 - 40 = 50 C
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_DOUBLE_EQ(50.0, v.value);
    EXPECT_STREQ("C", v.unit);
}

TEST(ObdDecode, IntakeAirTempZero) {
    const std::uint8_t d[] = {0x03, 0x41, 0x0F, 0x28};  // 40 - 40 = 0 C
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_DOUBLE_EQ(0.0, v.value);
    EXPECT_EQ("0", to_ds_string(v));
}

TEST(ObdDecode, ThrottleFullScale) {
    const std::uint8_t d[] = {0x03, 0x41, 0x11, 0xFF};  // 255*100/255 = 100 %
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_DOUBLE_EQ(100.0, v.value);
    EXPECT_EQ("100", to_ds_string(v));
}

TEST(ObdDecode, EngineLoadTrimmedDecimals) {
    const std::uint8_t d[] = {0x03, 0x41, 0x04, 0x7F};  // 127*100/255 = 49.80…
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_EQ("49.8", to_ds_string(v));
}

TEST(ObdDecode, Maf) {
    const std::uint8_t d[] = {0x04, 0x41, 0x10, 0x01, 0xF4};  // 500/100 = 5.00
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_DOUBLE_EQ(5.0, v.value);
    EXPECT_STREQ("g/s", v.unit);
    EXPECT_EQ("5", to_ds_string(v));
}

TEST(ObdDecode, FuelLevel) {
    const std::uint8_t d[] = {0x03, 0x41, 0x2F, 0x80};  // 128*100/255 = 50.19…
    auto v = decode_response(d, sizeof(d));
    ASSERT_TRUE(v.valid);
    EXPECT_EQ("50.2", to_ds_string(v));
}

// ── Malformed / unsupported ────────────────────────────────────────
TEST(ObdDecode, UnsupportedPidInvalid) {
    const std::uint8_t d[] = {0x03, 0x41, 0x99, 0x00};
    EXPECT_FALSE(decode_response(d, sizeof(d)).valid);
}

TEST(ObdDecode, WrongServiceIdInvalid) {
    const std::uint8_t d[] = {0x03, 0x7F, 0x0D, 0x50};  // negative response
    EXPECT_FALSE(decode_response(d, sizeof(d)).valid);
}

TEST(ObdDecode, TooShortInvalid) {
    const std::uint8_t d[] = {0x02, 0x41};
    EXPECT_FALSE(decode_response(d, sizeof(d)).valid);
}

TEST(ObdDecode, TwoBytepidMissingSecondByteInvalid) {
    const std::uint8_t d[] = {0x03, 0x41, 0x0C, 0x0F};  // RPM needs B
    EXPECT_FALSE(decode_response(d, sizeof(d)).valid);
}

TEST(ObdDecode, NullInvalid) {
    EXPECT_FALSE(decode_response(nullptr, 8).valid);
}

TEST(ObdDecode, ToDsStringEmptyWhenInvalid) {
    ObdValue v;  // valid == false
    EXPECT_EQ("", to_ds_string(v));
}

// ── DTC decoding (SAE J2012) ───────────────────────────────────────
TEST(ObdDtc, PowertrainCode) {
    // P0301: cat P(00), digits 0,3,0,1 → hi=0x03, lo=0x01.
    EXPECT_EQ("P0301", decode_dtc(0x03, 0x01));
}

TEST(ObdDtc, NetworkCode) {
    // U0123: cat U(11), 0,1,2,3 → hi=0xC1, lo=0x23.
    EXPECT_EQ("U0123", decode_dtc(0xC1, 0x23));
}

TEST(ObdDtc, ChassisCode) {
    // C0210: cat C(01), 0,2,1,0 → hi=0x42, lo=0x10.
    EXPECT_EQ("C0210", decode_dtc(0x42, 0x10));
}

TEST(ObdDtc, AllZeroIsNoDtc) {
    EXPECT_EQ("", decode_dtc(0x00, 0x00));
}
