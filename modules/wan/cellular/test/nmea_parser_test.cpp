#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <string>

#include "nmea_parser.hpp"

using namespace cellular;

// Wrap a sentence body in "$<body>*HH" with the correct XOR checksum.
static std::string framed(const std::string& body) {
    unsigned char sum = 0;
    for (char c : body) sum ^= static_cast<unsigned char>(c);
    char cs[8];
    std::snprintf(cs, sizeof(cs), "*%02X", sum);
    return "$" + body + cs;
}

// Canonical NMEA examples with documented checksums.
static const char* kGGA =
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
static const char* kRMC =
    "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";

TEST(Nmea, ChecksumValidation) {
    EXPECT_TRUE(nmea_checksum_ok(kGGA));
    EXPECT_TRUE(nmea_checksum_ok(kRMC));
    // corrupt a payload byte → checksum mismatch
    EXPECT_FALSE(nmea_checksum_ok(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48"));
    EXPECT_FALSE(nmea_checksum_ok("no dollar sign"));
}

TEST(Nmea, GgaPositionAltitudeSats) {
    GpsFix fix;
    ASSERT_TRUE(parse_gga(kGGA, fix));
    EXPECT_TRUE(fix.valid);
    EXPECT_NEAR(fix.lat, 48.1173, 1e-3);
    EXPECT_NEAR(fix.lon, 11.51667, 1e-3);
    EXPECT_NEAR(fix.alt_m, 545.4, 1e-3);
    EXPECT_EQ(fix.sats, 8);
    EXPECT_EQ(fix.quality, "3d");
}

TEST(Nmea, RmcSpeedCourse) {
    GpsFix fix;
    ASSERT_TRUE(parse_rmc(kRMC, fix));
    EXPECT_TRUE(fix.valid);
    EXPECT_NEAR(fix.lat, 48.1173, 1e-3);
    EXPECT_NEAR(fix.speed_kmh, 22.4 * 1.852, 1e-3);
    EXPECT_NEAR(fix.course_deg, 84.4, 1e-3);
}

TEST(Nmea, GgaThenRmcMerge) {
    GpsFix fix;
    ASSERT_TRUE(parse_gga(kGGA, fix));   // alt + sats + 3d
    ASSERT_TRUE(parse_rmc(kRMC, fix));   // speed + course, keeps 3d
    EXPECT_EQ(fix.quality, "3d");
    EXPECT_NEAR(fix.alt_m, 545.4, 1e-3);
    EXPECT_GT(fix.speed_kmh, 0.0);
}

TEST(Nmea, NoFixRmcVoid) {
    GpsFix fix;
    // status 'V' (void) — no position
    ASSERT_TRUE(parse_rmc(framed("GPRMC,123519,V,,,,,,,230394,,"), fix));
    EXPECT_FALSE(fix.valid);
    EXPECT_EQ(fix.quality, "none");
}

TEST(Nmea, RejectsWrongTypeOrChecksum) {
    GpsFix fix;
    EXPECT_FALSE(parse_gga(kRMC, fix));   // RMC fed to GGA parser
    EXPECT_FALSE(parse_rmc(kGGA, fix));   // GGA fed to RMC parser
}

TEST(Qgpsloc, DecimalModeFix) {
    GpsFix fix;
    ASSERT_TRUE(parse_qgpsloc(
        "+QGPSLOC: 092204.0,31.22246,121.35372,1.2,57.1,3,45.0,12.5,6.7,200520,06", fix));
    EXPECT_TRUE(fix.valid);
    EXPECT_EQ(fix.quality, "3d");
    EXPECT_NEAR(fix.lat, 31.22246, 1e-5);
    EXPECT_NEAR(fix.lon, 121.35372, 1e-5);
    EXPECT_NEAR(fix.alt_m, 57.1, 1e-3);
    EXPECT_NEAR(fix.course_deg, 45.0, 1e-3);
    EXPECT_NEAR(fix.speed_kmh, 12.5, 1e-3);
    EXPECT_EQ(fix.sats, 6);
}

TEST(Qgpsloc, NegativeDecimalDegrees) {
    GpsFix fix;
    ASSERT_TRUE(parse_qgpsloc(
        "+QGPSLOC: 010101.0,-33.86880,-151.20930,0.8,15.0,2,0.0,0.0,0.0,010120,05", fix));
    EXPECT_EQ(fix.quality, "2d");
    EXPECT_NEAR(fix.lat, -33.86880, 1e-5);
    EXPECT_NEAR(fix.lon, -151.20930, 1e-5);
}

TEST(Qgpsloc, NoFixIsRejected) {
    GpsFix fix;
    EXPECT_FALSE(parse_qgpsloc("+CME ERROR: 516", fix));   // not fixed yet
    EXPECT_FALSE(parse_qgpsloc("+QGPSLOC: 0.0,,,,,0", fix));
}
