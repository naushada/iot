#include <gtest/gtest.h>
#include <algorithm>

#include "cell_state.hpp"

using namespace cellular;

static std::string val(const std::vector<KV>& kv, const std::string& k) {
    for (auto& e : kv) if (e.key == k) return e.value;
    return "<absent>";
}
static bool has(const std::vector<KV>& kv, const std::string& k) {
    return std::any_of(kv.begin(), kv.end(), [&](const KV& e){ return e.key == k; });
}

TEST(CellState, EmptyEmitsNothing) {
    CellularState st;
    EXPECT_TRUE(st.to_kv().empty());
}

TEST(CellState, CellularFieldsPublished) {
    CellularState st;
    st.set_state("connected");
    st.set_signal(parse_csq("+CSQ: 20,0"));        // -73 dBm
    st.set_operator(parse_cops("+COPS: 0,0,\"Telekom\",7"));
    st.set_reg(parse_creg("+CREG: 0,5"));          // roaming
    st.set_ip("10.20.30.40");
    st.set_iccid(parse_iccid("+QCCID: 8944500207123456789"));

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "cell.state"), "connected");
    EXPECT_EQ(val(kv, "cell.operator"), "Telekom");
    EXPECT_EQ(val(kv, "cell.tech"), "4G");
    EXPECT_EQ(val(kv, "cell.reg"), "roaming");
    EXPECT_EQ(val(kv, "cell.signal.dbm"), "-73");
    EXPECT_EQ(val(kv, "cell.ip"), "10.20.30.40");
    EXPECT_EQ(val(kv, "cell.iccid"), "8944500207123456789");
    EXPECT_TRUE(has(kv, "cell.version"));
    // no GPS set → no gps.* keys
    EXPECT_FALSE(has(kv, "gps.fix"));
}

TEST(CellState, GpsFixPublished) {
    CellularState st;
    GpsFix fix;
    parse_gga("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", fix);
    st.set_gps(fix);

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "gps.fix"), "3d");
    EXPECT_EQ(val(kv, "gps.sats"), "8");
    EXPECT_NE(val(kv, "gps.lat"), "<absent>");
    EXPECT_NE(val(kv, "gps.lon"), "<absent>");
    EXPECT_TRUE(has(kv, "gps.version"));
    // no cellular set → no cell.* keys
    EXPECT_FALSE(has(kv, "cell.state"));
}

TEST(CellState, VersionBumpsOnChange) {
    CellularState st;
    st.set_state("init");
    const std::string v1 = val(st.to_kv(), "cell.version");
    st.set_state("connected");
    const std::string v2 = val(st.to_kv(), "cell.version");
    EXPECT_NE(v1, v2);
}

TEST(CellState, SmsPublished) {
    CellularState st;
    SmsMessage m;
    m.sender = "+1234"; m.text = "hello"; m.scts = "2026-07-04T12:00:00";
    st.set_sms(m);

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "sms.last.sender"), "+1234");
    EXPECT_EQ(val(kv, "sms.last.text"), "hello");
    EXPECT_EQ(val(kv, "sms.last.ts"), "2026-07-04T12:00:00");
    EXPECT_EQ(val(kv, "sms.count"), "1");
    EXPECT_TRUE(has(kv, "sms.version"));
    EXPECT_FALSE(has(kv, "cell.state"));       // nothing else set

    st.set_sms(m);                             // second message → count bumps
    EXPECT_EQ(val(st.to_kv(), "sms.count"), "2");
}

TEST(CellState, InvalidSignalIgnored) {
    CellularState st;
    st.set_state("searching");
    st.set_signal(parse_csq("+CSQ: 99,99"));   // unknown → not stored
    auto kv = st.to_kv();
    EXPECT_FALSE(has(kv, "cell.signal.dbm"));
}
