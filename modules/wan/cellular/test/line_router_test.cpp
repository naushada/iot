#include <gtest/gtest.h>
#include <algorithm>

#include "line_router.hpp"

using namespace cellular;

static std::string val(const std::vector<KV>& kv, const std::string& k) {
    for (auto& e : kv) if (e.key == k) return e.value;
    return "<absent>";
}

TEST(LineAssembler, ReassemblesAcrossChunks) {
    LineAssembler la;
    auto a = la.feed("+CSQ: 2");
    EXPECT_TRUE(a.empty());                       // partial, no newline yet
    auto b = la.feed("0,0\r\n+COPS: 0,0,\"X\",7\r\n");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], "+CSQ: 20,0");
    EXPECT_EQ(b[1], "+COPS: 0,0,\"X\",7");
}

TEST(LineAssembler, DropsEmptyLinesAndCr) {
    LineAssembler la;
    auto lines = la.feed("\r\nOK\r\n\r\n+CME ERROR: 30\r\n");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "OK");
    EXPECT_EQ(lines[1], "+CME ERROR: 30");
}

TEST(DispatchAt, RoutesKnownResponses) {
    CellularState st;
    EXPECT_TRUE(dispatch_at_line("+CSQ: 20,0", st));
    EXPECT_TRUE(dispatch_at_line("+COPS: 0,0,\"Orange\",2", st));
    EXPECT_TRUE(dispatch_at_line("+CEREG: 0,1", st));
    EXPECT_TRUE(dispatch_at_line("+CGPADDR: 1,\"10.0.0.5\"", st));
    EXPECT_TRUE(dispatch_at_line("+QCCID: 8944500207123456789", st));
    EXPECT_FALSE(dispatch_at_line("OK", st));
    EXPECT_FALSE(dispatch_at_line("RING", st));

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "cell.signal.dbm"), "-73");
    EXPECT_EQ(val(kv, "cell.operator"), "Orange");
    EXPECT_EQ(val(kv, "cell.tech"), "3G");
    EXPECT_EQ(val(kv, "cell.reg"), "home");
    EXPECT_EQ(val(kv, "cell.ip"), "10.0.0.5");
    EXPECT_EQ(val(kv, "cell.iccid"), "8944500207123456789");
}

// The Sierra WP7702 answers AT+ICCID with a BARE "ICCID: <digits>" — no leading
// '+'. Matching only +QCCID:/+CCID: left cell.iccid empty on every WP module.
TEST(DispatchAt, AcceptsAllIccidPrefixes) {
    for (const char* line : {"+QCCID: 8991000925010294882",
                             "+CCID: 8991000925010294882",
                             "+ICCID: 8991000925010294882",
                             "ICCID: 8991000925010294882"}) {
        CellularState st;
        EXPECT_TRUE(dispatch_at_line(line, st)) << line;
        EXPECT_EQ(val(st.to_kv(), "cell.iccid"), "8991000925010294882") << line;
    }
}

// The carrier resolvers land in cell.dns (AT+CGCONTRDP=1).
TEST(DispatchAt, PublishesCarrierDns) {
    CellularState st;
    EXPECT_TRUE(dispatch_at_line(
        "+CGCONTRDP: 1,5,airtelgprs.com,100.75.219.215,,117.96.122.74,59.144.127.117", st));
    EXPECT_EQ(val(st.to_kv(), "cell.dns"), "117.96.122.74,59.144.127.117");
}

TEST(DispatchNmea, MergesGgaAndRmc) {
    CellularState st;
    GpsFix acc;
    EXPECT_TRUE(dispatch_nmea_line(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", acc, st));
    EXPECT_TRUE(dispatch_nmea_line(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", acc, st));
    EXPECT_FALSE(dispatch_nmea_line("$GPGSV,3,1,11,01,40,083,46*7B", acc, st));  // unhandled

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "gps.fix"), "3d");
    EXPECT_EQ(val(kv, "gps.sats"), "8");
    EXPECT_NE(val(kv, "gps.speed"), "<absent>");
}

TEST(DispatchAt, RoutesQgpslocToGps) {
    CellularState st;
    EXPECT_TRUE(dispatch_at_line(
        "+QGPSLOC: 092204.0,31.22246,121.35372,1.2,57.1,3,45.0,12.5,6.7,200520,06", st));
    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "gps.fix"), "3d");
    EXPECT_EQ(val(kv, "gps.sats"), "6");
    EXPECT_NE(val(kv, "gps.lat"), "<absent>");
    // a no-fix +QGPSLOC is still "handled" (returns true) but sets no fix
    CellularState st2;
    EXPECT_TRUE(dispatch_at_line("+QGPSLOC: 0.0,,,,,0", st2));
    EXPECT_TRUE(st2.to_kv().empty());
}

TEST(DispatchNmea, RejectsBadChecksum) {
    CellularState st;
    GpsFix acc;
    EXPECT_FALSE(dispatch_nmea_line(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00", acc, st));
}
