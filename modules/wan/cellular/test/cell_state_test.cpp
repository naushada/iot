#include <gtest/gtest.h>
#include <algorithm>

#include <nlohmann/json.hpp>

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

/// The PDP table reaches ds as a JSON array so the device-ui can show WHICH
/// contexts the modem holds — the eSIM's profile sits alongside ours, and with a
/// single APN string on screen there was no way to tell them apart.
TEST(CellState, ApnProfilesAreAJsonArray) {
    CellularState st;
    PdpProfile ours; ours.cid = 1; ours.type = "IP"; ours.apn = "airtelgprs.com";
    PdpProfile esim; esim.cid = 2; esim.type = "IP"; esim.apn = "iot.swir";
    st.set_apn_profiles({ours, esim});
    st.set_apn("airtelgprs.com");   // the daemon feeds it OUR cid's apn

    auto arr = nlohmann::json::parse(val(st.to_kv(), "cell.apn.profiles"));
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr[0]["cid"], 1);
    EXPECT_EQ(arr[0]["apn"], "airtelgprs.com");
    EXPECT_EQ(arr[1]["cid"], 2);
    EXPECT_EQ(arr[1]["apn"], "iot.swir");
    // The eSIM's context must NOT bleed into the reported APN.
    EXPECT_EQ(val(st.to_kv(), "cell.apn.current"), "airtelgprs.com");
}

TEST(CellState, SmsInboxIsNewestFirstJsonArray) {
    CellularState st;
    SmsMessage a; a.sender = "+111"; a.text = "first";  a.scts = "2026-07-04T10:00:00";
    SmsMessage b; b.sender = "+222"; b.text = "second"; b.scts = "2026-07-04T11:00:00";
    st.set_sms(a);
    st.set_sms(b);

    auto inbox = nlohmann::json::parse(val(st.to_kv(), "sms.inbox"));
    ASSERT_TRUE(inbox.is_array());
    ASSERT_EQ(inbox.size(), 2u);
    EXPECT_EQ(inbox[0]["from"], "+222");       // newest first
    EXPECT_EQ(inbox[0]["text"], "second");
    EXPECT_EQ(inbox[1]["from"], "+111");
}

TEST(CellState, SmsInboxEscapesAwkwardText) {
    CellularState st;
    SmsMessage m; m.sender = "+1"; m.text = "quote\" back\\slash\nnewline"; m.scts = "t";
    st.set_sms(m);
    // Round-trips through JSON without corruption (nlohmann handles escaping).
    auto inbox = nlohmann::json::parse(val(st.to_kv(), "sms.inbox"));
    EXPECT_EQ(inbox[0]["text"], "quote\" back\\slash\nnewline");
}

TEST(CellState, SmsInboxBoundedToTwenty) {
    CellularState st;
    for (int i = 0; i < 25; ++i) {
        SmsMessage m; m.sender = "+" + std::to_string(i); m.text = "m"; m.scts = "t";
        st.set_sms(m);
    }
    auto inbox = nlohmann::json::parse(val(st.to_kv(), "sms.inbox"));
    EXPECT_EQ(inbox.size(), 20u);              // capped
    EXPECT_EQ(inbox[0]["from"], "+24");        // newest kept
    EXPECT_EQ(val(st.to_kv(), "sms.count"), "25");  // count still totals all
}

TEST(CellState, SeedInboxRestoresHistory) {
    CellularState st;
    const std::string persisted =
        R"([{"ts":"t2","from":"+22","text":"two"},{"ts":"t1","from":"+11","text":"one"}])";
    st.seed_inbox(persisted, 7);

    auto kv = st.to_kv();
    EXPECT_EQ(val(kv, "sms.count"), "7");          // count restored
    EXPECT_EQ(val(kv, "sms.last.sender"), "+22");  // newest becomes last.*
    auto inbox = nlohmann::json::parse(val(kv, "sms.inbox"));
    ASSERT_EQ(inbox.size(), 2u);
    EXPECT_EQ(inbox[0]["from"], "+22");

    // A new message prepends to the restored history rather than clobbering it.
    SmsMessage n; n.sender = "+33"; n.text = "three"; n.scts = "t3";
    st.set_sms(n);
    inbox = nlohmann::json::parse(val(st.to_kv(), "sms.inbox"));
    EXPECT_EQ(inbox.size(), 3u);
    EXPECT_EQ(inbox[0]["from"], "+33");
    EXPECT_EQ(val(st.to_kv(), "sms.count"), "8");
}

TEST(CellState, SeedInboxToleratesGarbage) {
    CellularState st;
    st.seed_inbox("not json", 3);              // must not throw
    EXPECT_EQ(val(st.to_kv(), "sms.count"), "3");
}

TEST(CellState, InvalidSignalIgnored) {
    CellularState st;
    st.set_state("searching");
    st.set_signal(parse_csq("+CSQ: 99,99"));   // unknown → not stored
    auto kv = st.to_kv();
    EXPECT_FALSE(has(kv, "cell.signal.dbm"));
}
