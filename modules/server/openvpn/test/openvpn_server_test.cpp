/// Unit tests for the OpenVPN server config renderer (pure).

#include <gtest/gtest.h>

#include <ctime>
#include <string>

#include "openvpn_server.hpp"

using server::openvpn::OpenVpnServer;
using server::openvpn::OpenVpnServerConfig;
using server::openvpn::build_server_config;
using server::openvpn::cidr_to_net_mask;
using server::openvpn::parse_status_routing_table;

namespace {

// asctime("%a %b %d %H:%M:%S %Y") → epoch, via the same mktime() the parser
// uses, so the freshness tests are timezone-independent (both interpret the
// timestamp in the host's local zone).
std::time_t epoch_of(const char* ts) {
    std::tm tm{};
    ::strptime(ts, "%a %b %d %H:%M:%S %Y", &tm);
    tm.tm_isdst = -1;
    return ::mktime(&tm);
}

// Wrap routing-table rows in a realistic v1 `status` dump (incl. a CLIENT LIST
// section that must NOT be parsed, and a GLOBAL STATS terminator).
std::string mk_status(const std::string& routing_rows) {
    return
        "OpenVPN CLIENT LIST\n"
        "Updated,Wed Jun 25 02:30:10 2026\n"
        "Common Name,Real Address,Bytes Received,Bytes Sent,Connected Since\n"
        "rpiNOPE@cloud.local,9.9.9.9:1111,1,2,Wed Jun 25 02:00:00 2026\n"
        "ROUTING TABLE\n"
        "Virtual Address,Common Name,Real Address,Last Ref\n"
        + routing_rows +
        "GLOBAL STATS\n"
        "Max bcast/mcast queue length,0\n"
        "END\n";
}

TEST(CidrToNetMask, slash24) {
    std::string net, mask;
    ASSERT_TRUE(cidr_to_net_mask("10.9.0.0/24", net, mask));
    EXPECT_EQ(net, "10.9.0.0");
    EXPECT_EQ(mask, "255.255.255.0");
}

TEST(CidrToNetMask, slash16) {
    std::string net, mask;
    ASSERT_TRUE(cidr_to_net_mask("172.16.0.0/16", net, mask));
    EXPECT_EQ(net, "172.16.0.0");
    EXPECT_EQ(mask, "255.255.0.0");
}

TEST(CidrToNetMask, malformed) {
    std::string net, mask;
    EXPECT_FALSE(cidr_to_net_mask("10.9.0.0", net, mask));   // no prefix
    EXPECT_FALSE(cidr_to_net_mask("10.9.0.0/99", net, mask)); // bad prefix
}

TEST(BuildServerConfig, rendersServerLines) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.port = 1194;
    const std::string conf = build_server_config(c);
    EXPECT_NE(conf.find("mode server"), std::string::npos);
    EXPECT_NE(conf.find("server 10.9.0.0 255.255.255.0"), std::string::npos);
    EXPECT_NE(conf.find("port 1194"), std::string::npos);
    EXPECT_NE(conf.find("dh none"), std::string::npos);
    EXPECT_NE(conf.find("ca   /etc/iot/vpn/ca/ca.crt"), std::string::npos);
    EXPECT_NE(conf.find("management 127.0.0.1"), std::string::npos);
}

TEST(BuildServerConfig, badSubnetReturnsEmpty) {
    OpenVpnServerConfig c;
    c.subnet = "not-a-cidr";
    EXPECT_TRUE(build_server_config(c).empty());
}

TEST(BuildServerConfig, clientConfigDirEmittedOnlyWhenSet) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/16";
    // Default (no ccd_dir): no client-config-dir line — single-tenant unchanged.
    EXPECT_EQ(build_server_config(c).find("client-config-dir"),
              std::string::npos);
    // Multi-tenant (P3c): the directive appears when configured.
    c.ccd_dir = "/etc/iot/vpn/ccd";
    EXPECT_NE(build_server_config(c).find("client-config-dir /etc/iot/vpn/ccd"),
              std::string::npos);
}

TEST(BuildServerConfig, tcpProtoGetsServerSuffix) {
    // Operator-facing proto is the base form "tcp"; the server *socket* must
    // LISTEN, so the config must render "proto tcp-server" (not bare "proto tcp",
    // which OpenVPN treats as a client and never listens).
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.proto  = "tcp";
    const std::string conf = build_server_config(c);
    EXPECT_NE(conf.find("proto tcp-server"), std::string::npos);
    EXPECT_EQ(conf.find("proto tcp\n"), std::string::npos);   // never the bare form
}

TEST(BuildServerConfig, udpProtoUnchanged) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.proto  = "udp";
    EXPECT_NE(build_server_config(c).find("proto udp\n"), std::string::npos);
}

TEST(BuildServerConfig, legacyTcpServerPassesThrough) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.proto  = "tcp-server";                 // legacy stored value
    const std::string conf = build_server_config(c);
    EXPECT_NE(conf.find("proto tcp-server"), std::string::npos);
    EXPECT_EQ(conf.find("tcp-server-server"), std::string::npos);   // not doubled
}

TEST(BuildServerConfig, pushesDnsWhenSet) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.dns = "1.1.1.1";
    const std::string conf = build_server_config(c);
    EXPECT_NE(conf.find("push \"dhcp-option DNS 1.1.1.1\""), std::string::npos);
}

TEST(BuildServerConfig, noDnsPushWhenEmpty) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";   // c.dns left empty
    const std::string conf = build_server_config(c);
    EXPECT_EQ(conf.find("dhcp-option DNS"), std::string::npos);
}

TEST(BuildServerConfig, crlVerifyEmittedWhenSet) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.crl = "/etc/iot/vpn/ca/crl.pem";
    const std::string conf = build_server_config(c);
    EXPECT_NE(conf.find("crl-verify /etc/iot/vpn/ca/crl.pem"), std::string::npos);
}

TEST(BuildServerConfig, noCrlVerifyWhenUnset) {
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";   // c.crl left empty
    const std::string conf = build_server_config(c);
    EXPECT_EQ(conf.find("crl-verify"), std::string::npos);
}

TEST(Reconfigure, noopWhenRenderedConfigUnchanged) {
    // A redundant ds write (same effective config) must NOT report a change,
    // so the cloud hot-reload path doesn't bounce a healthy tunnel.
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.proto  = "tcp";
    OpenVpnServer srv(c);              // never started → no child to manage
    OpenVpnServerConfig same = c;
    EXPECT_FALSE(srv.reconfigure(same));
}

TEST(Reconfigure, reportsChangeWhenProtoFlips) {
    // The motivating case: operator flips cloud.vpn.proto tcp→udp. The rendered
    // config differs (proto tcp-server → proto udp), so reconfigure() reports a
    // change and the caller restarts the server on the new socket.
    OpenVpnServerConfig c;
    c.subnet = "10.9.0.0/24";
    c.proto  = "tcp";
    OpenVpnServer srv(c);
    OpenVpnServerConfig flipped = c;
    flipped.proto = "udp";
    EXPECT_TRUE(srv.reconfigure(flipped));
    // Idempotent: re-applying the now-current config is a no-op.
    EXPECT_FALSE(srv.reconfigure(flipped));
}

// ───────────────── parse_status_routing_table ──────────────────────

TEST(RoutingTable, parsesFreshRoutesAndIgnoresClientList) {
    const char* T = "Wed Jun 25 02:30:00 2026";
    std::string rows =
        std::string("10.9.0.3,rpi6556e041@cloud.local,103.248.74.149:54321,") + T + "\n"
        "10.9.0.5,rpiABCD@cloud.local,8.8.8.8:5000," + T + "\n";
    auto m = parse_status_routing_table(mk_status(rows), epoch_of(T) + 5, 30);
    ASSERT_EQ(2u, m.size());
    EXPECT_EQ("10.9.0.3", m["6556e041"].vip);
    EXPECT_EQ("103.248.74.149", m["6556e041"].wan_ip);   // :port stripped
    EXPECT_EQ("10.9.0.5", m["ABCD"].vip);
    EXPECT_EQ(0u, m.count("NOPE"));                       // CLIENT LIST not parsed
}

TEST(RoutingTable, dropsStaleRouteKeepsFresh) {
    const char* FRESH = "Wed Jun 25 02:30:00 2026";
    const char* STALE = "Wed Jun 25 02:25:00 2026";      // 5 min earlier
    std::string rows =
        std::string("10.9.0.3,rpiFRESH@cloud.local,1.1.1.1:1,") + FRESH + "\n"
        "10.9.0.9,rpiSTALE@cloud.local,2.2.2.2:2," + STALE + "\n";
    // now = 5s after FRESH → FRESH age 5s (kept), STALE age 305s (dropped, >30s).
    auto m = parse_status_routing_table(mk_status(rows), epoch_of(FRESH) + 5, 30);
    ASSERT_EQ(1u, m.size());
    EXPECT_EQ(1u, m.count("FRESH"));
    EXPECT_EQ(0u, m.count("STALE"));
}

TEST(RoutingTable, keepsUnparseableLastRef_failSafe) {
    // A Last Ref the parser can't read must NEVER hide a connected device.
    std::string rows = "10.9.0.3,rpiX@cloud.local,1.1.1.1:1,not-a-date\n";
    auto m = parse_status_routing_table(mk_status(rows), epoch_of("Wed Jun 25 02:30:00 2026"), 30);
    ASSERT_EQ(1u, m.size());
    EXPECT_EQ("10.9.0.3", m["X"].vip);
}

TEST(RoutingTable, noFilterWhenMaxAgeNonPositive) {
    const char* STALE = "Wed Jun 25 02:25:00 2026";
    std::string rows = std::string("10.9.0.3,rpiOLD@cloud.local,1.1.1.1:1,") + STALE + "\n";
    // max_age <= 0 disables the freshness filter — even an ancient route stays.
    auto m = parse_status_routing_table(mk_status(rows), epoch_of(STALE) + 999999, 0);
    EXPECT_EQ(1u, m.size());
}

TEST(RoutingTable, emptyWhenNoRoutingSection) {
    EXPECT_TRUE(parse_status_routing_table("no routing table here\nEND\n", 0, 30).empty());
    EXPECT_TRUE(parse_status_routing_table("", 0, 30).empty());
}

}  // namespace
