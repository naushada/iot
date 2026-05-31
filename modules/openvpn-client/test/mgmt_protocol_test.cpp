/// Table-driven tests for the openvpn(8) management-interface parser.
///
/// Fixtures are byte-for-byte captures (or close approximations) of
/// what `openvpn --management 127.0.0.1 7505` writes on the mgmt
/// socket. The parser is pure logic so we drive it with raw strings
/// and assert on the emitted Event stream.

#include <gtest/gtest.h>

#include "mgmt_protocol.hpp"

using openvpn_client::mgmt::Event;
using openvpn_client::mgmt::Parser;

namespace {

/// Drain every available event from a parser into a vector — keeps
/// the tests linear instead of a while-loop boilerplate per case.
std::vector<Event> drain(Parser& p) {
    std::vector<Event> out;
    while (auto ev = p.next()) out.push_back(*std::move(ev));
    return out;
}

} // namespace

/* ─────────────────────────── Banner ──────────────────────────────── */

TEST(MgmtParser, BannerLineProducesOneBannerEvent) {
    Parser p;
    p.feed(">INFO:OpenVPN Management Interface Version 1 -- "
           "type 'help' for more info\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::Banner, evs[0].kind);
    EXPECT_EQ("OpenVPN Management Interface Version 1 -- "
              "type 'help' for more info", evs[0].raw);
    EXPECT_EQ(0u, p.buffer_size());
}

/* ─────────────────────────── STATE ───────────────────────────────── */

TEST(MgmtParser, StateConnectedFieldsAreCommaSplit) {
    Parser p;
    p.feed(">STATE:1701420000,CONNECTED,SUCCESS,10.8.0.6,"
           "vpn.example.com,1194,,\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::State, evs[0].kind);
    ASSERT_EQ(8u, evs[0].fields.size());
    EXPECT_EQ("1701420000",      evs[0].fields[0]);
    EXPECT_EQ("CONNECTED",       evs[0].fields[1]);
    EXPECT_EQ("SUCCESS",         evs[0].fields[2]);
    EXPECT_EQ("10.8.0.6",        evs[0].fields[3]);
    EXPECT_EQ("vpn.example.com", evs[0].fields[4]);
    EXPECT_EQ("1194",            evs[0].fields[5]);
    EXPECT_EQ("",                evs[0].fields[6]);
    EXPECT_EQ("",                evs[0].fields[7]);
}

TEST(MgmtParser, StateAssignIpCarriesAssignedIpInFieldThree) {
    Parser p;
    p.feed(">STATE:1701420001,ASSIGN_IP,,10.8.0.6,,,,,\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::State, evs[0].kind);
    ASSERT_GE(evs[0].fields.size(), 4u);
    EXPECT_EQ("ASSIGN_IP", evs[0].fields[1]);
    EXPECT_EQ("10.8.0.6",  evs[0].fields[3]);
}

/* ─────────────────────────── PUSH_REPLY ──────────────────────────── */

TEST(MgmtParser, PushReplyCommaSplitsOptions) {
    Parser p;
    p.feed(">PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,"
           "topology subnet,ifconfig 10.8.0.6 255.255.255.0,"
           "cipher AES-256-GCM,peer-id 0\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::PushReply, evs[0].kind);
    ASSERT_EQ(6u, evs[0].fields.size());
    EXPECT_EQ("dhcp-option DNS 1.1.1.1",          evs[0].fields[0]);
    EXPECT_EQ("route-gateway 10.8.0.1",           evs[0].fields[1]);
    EXPECT_EQ("topology subnet",                  evs[0].fields[2]);
    EXPECT_EQ("ifconfig 10.8.0.6 255.255.255.0",  evs[0].fields[3]);
    EXPECT_EQ("cipher AES-256-GCM",               evs[0].fields[4]);
    EXPECT_EQ("peer-id 0",                        evs[0].fields[5]);
}

TEST(MgmtParser, SplitPushOptionSeparatesNameAndValue) {
    using openvpn_client::mgmt::split_push_option;

    auto [n1, v1] = split_push_option("dhcp-option DNS 1.1.1.1");
    EXPECT_EQ("dhcp-option",     n1);
    EXPECT_EQ("DNS 1.1.1.1",     v1);

    auto [n2, v2] = split_push_option("ifconfig 10.8.0.6 255.255.255.0");
    EXPECT_EQ("ifconfig",                 n2);
    EXPECT_EQ("10.8.0.6 255.255.255.0",   v2);

    auto [n3, v3] = split_push_option("redirect-gateway");
    EXPECT_EQ("redirect-gateway", n3);
    EXPECT_EQ("",                 v3);

    // Leading whitespace from a stray "," tolerated.
    auto [n4, v4] = split_push_option("  cipher AES-256-GCM");
    EXPECT_EQ("cipher",          n4);
    EXPECT_EQ("AES-256-GCM",     v4);
}

/* ─────────────────────────── ByteCount ───────────────────────────── */

TEST(MgmtParser, ByteCountFieldsAreIntegersAsStrings) {
    Parser p;
    p.feed(">BYTECOUNT:12345,67890\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::ByteCount, evs[0].kind);
    ASSERT_EQ(2u, evs[0].fields.size());
    EXPECT_EQ("12345", evs[0].fields[0]);
    EXPECT_EQ("67890", evs[0].fields[1]);
}

/* ─────────────────────────── Command replies ─────────────────────── */

TEST(MgmtParser, SuccessReplyKeepsBodyVerbatim) {
    Parser p;
    p.feed("SUCCESS: pid=12345\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::SuccessReply, evs[0].kind);
    EXPECT_EQ(" pid=12345", evs[0].raw);   // includes the leading space
}

TEST(MgmtParser, ErrorReplyClassifiesUnknownCommand) {
    Parser p;
    p.feed("ERROR: unknown command, enter 'help' for more options\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::ErrorReply, evs[0].kind);
}

TEST(MgmtParser, EndMarkerIsItsOwnEvent) {
    Parser p;
    p.feed("END\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::EndMarker, evs[0].kind);
}

/* ─────────────────────────── Edge cases ──────────────────────────── */

TEST(MgmtParser, MultipleEventsDeliveredInOneFeed) {
    Parser p;
    p.feed(">INFO:Hello\n>STATE:0,CONNECTING,,,,,\n>HOLD:Waiting for hold release.\n");
    auto evs = drain(p);
    ASSERT_EQ(3u, evs.size());
    EXPECT_EQ(Event::Kind::Banner, evs[0].kind);
    EXPECT_EQ(Event::Kind::State,  evs[1].kind);
    EXPECT_EQ(Event::Kind::Hold,   evs[2].kind);
}

TEST(MgmtParser, PartialLineHeldUntilNewline) {
    Parser p;
    p.feed(">STATE:0,CONNECT");   // partial
    EXPECT_EQ(0u, drain(p).size());
    EXPECT_GT(p.buffer_size(), 0u);

    p.feed("ING,,,,,\n");          // completes the line
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::State, evs[0].kind);
    EXPECT_EQ("CONNECTING", evs[0].fields[1]);
    EXPECT_EQ(0u, p.buffer_size());
}

TEST(MgmtParser, CrlfLineEndingsTolerated) {
    Parser p;
    p.feed(">STATE:0,WAIT,,,,,\r\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::State, evs[0].kind);
    // Trailing CR stripped — fields[5] is "" not "\r".
    EXPECT_EQ("WAIT", evs[0].fields[1]);
    EXPECT_EQ("",     evs[0].fields[5]);
}

TEST(MgmtParser, EmptyLineDropped) {
    Parser p;
    p.feed("\n");
    EXPECT_EQ(0u, drain(p).size());
}

TEST(MgmtParser, UnknownAsyncEventClassifiedAsUnknown) {
    Parser p;
    p.feed(">FANCY-NEW-EVENT:something\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::Unknown, evs[0].kind);
    EXPECT_EQ("something", evs[0].raw);
}

TEST(MgmtParser, NonPrefixedLineIsDataLine) {
    // e.g. response to `status` command: the rows between SUCCESS:
    // and END are bare data lines.
    Parser p;
    p.feed("OpenVPN STATISTICS\n");
    auto evs = drain(p);
    ASSERT_EQ(1u, evs.size());
    EXPECT_EQ(Event::Kind::DataLine, evs[0].kind);
    EXPECT_EQ("OpenVPN STATISTICS", evs[0].raw);
}

TEST(MgmtParser, RealisticOpenvpnConnectStreamProducesExpectedSequence) {
    // Real-ish capture of the early frames an openvpn client emits
    // on the mgmt socket between connect + first ASSIGN_IP. Caller's
    // FSM (D6) would walk this stream and write back state/IP keys.
    Parser p;
    p.feed(
        ">INFO:OpenVPN Management Interface Version 1 -- type 'help' for more info\n"
        ">HOLD:Waiting for hold release.\n"
        ">STATE:1701420000,CONNECTING,,,,,\n"
        ">STATE:1701420001,WAIT,,,,,\n"
        ">STATE:1701420002,AUTH,,,,,\n"
        ">STATE:1701420003,GET_CONFIG,,,,,\n"
        ">PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,"
        "ifconfig 10.8.0.6 255.255.255.0,cipher AES-256-GCM\n"
        ">STATE:1701420004,ASSIGN_IP,,10.8.0.6,,,,,\n"
        ">STATE:1701420005,CONNECTED,SUCCESS,10.8.0.6,"
        "vpn.example.com,1194,,\n");

    auto evs = drain(p);
    ASSERT_EQ(9u, evs.size());
    EXPECT_EQ(Event::Kind::Banner,    evs[0].kind);
    EXPECT_EQ(Event::Kind::Hold,      evs[1].kind);
    EXPECT_EQ(Event::Kind::State,     evs[2].kind);
    EXPECT_EQ("CONNECTING",           evs[2].fields[1]);
    EXPECT_EQ(Event::Kind::State,     evs[3].kind);
    EXPECT_EQ("WAIT",                 evs[3].fields[1]);
    EXPECT_EQ(Event::Kind::State,     evs[4].kind);
    EXPECT_EQ("AUTH",                 evs[4].fields[1]);
    EXPECT_EQ(Event::Kind::State,     evs[5].kind);
    EXPECT_EQ("GET_CONFIG",           evs[5].fields[1]);
    EXPECT_EQ(Event::Kind::PushReply, evs[6].kind);
    ASSERT_EQ(4u, evs[6].fields.size());
    EXPECT_EQ("ifconfig 10.8.0.6 255.255.255.0", evs[6].fields[2]);
    EXPECT_EQ(Event::Kind::State,     evs[7].kind);
    EXPECT_EQ("ASSIGN_IP",            evs[7].fields[1]);
    EXPECT_EQ("10.8.0.6",             evs[7].fields[3]);
    EXPECT_EQ(Event::Kind::State,     evs[8].kind);
    EXPECT_EQ("CONNECTED",            evs[8].fields[1]);
}
