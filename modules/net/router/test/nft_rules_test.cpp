/// Table-driven tests for the pure nft ruleset generator.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nft_rules.hpp"

using net_router::nft::build_nft_ruleset;
using net_router::nft::CustomRule;
using net_router::nft::parse_custom_rules;
using net_router::nft::parse_forward_ports;
using net_router::nft::State;

/* ─────────────────────── parse_forward_ports ─────────────────────── */

TEST(ParseForwardPorts, DefaultListYields3Ports) {
    auto ps = parse_forward_ports("80,443,5684");
    ASSERT_EQ(3u, ps.size());
    EXPECT_EQ(80,   ps[0]);
    EXPECT_EQ(443,  ps[1]);
    EXPECT_EQ(5684, ps[2]);
}

TEST(ParseForwardPorts, ToleratesWhitespaceAndEmptyTokens) {
    auto ps = parse_forward_ports(" 80 , , 443,5684 ,");
    ASSERT_EQ(3u, ps.size());
}

TEST(ParseForwardPorts, SkipsOutOfRangeAndJunk) {
    auto ps = parse_forward_ports("80,99999,abc,443,0");
    ASSERT_EQ(2u, ps.size());   // 99999, abc, 0 dropped
    EXPECT_EQ(80,  ps[0]);
    EXPECT_EQ(443, ps[1]);
}

/* ─────────────────────── parse_custom_rules ──────────────────────── */

TEST(ParseCustomRules, EmptyArrayProducesEmptyVector) {
    std::string err;
    auto rs = parse_custom_rules("[]", &err);
    EXPECT_TRUE(rs.empty());
    EXPECT_TRUE(err.empty());
}

TEST(ParseCustomRules, ValidObjectsRoundTrip) {
    std::string json =
        "[{\"action\":\"drop\",\"proto\":\"tcp\",\"dport\":23},"
        " {\"action\":\"forward\",\"proto\":\"udp\",\"dport\":5683,"
        "  \"to_ip\":\"10.8.0.6\",\"to_port\":5683}]";
    auto rs = parse_custom_rules(json);
    ASSERT_EQ(2u, rs.size());
    EXPECT_EQ("drop",     rs[0].action);
    EXPECT_EQ("tcp",      rs[0].proto);
    EXPECT_EQ(23u,        rs[0].dport);
    EXPECT_EQ("forward",  rs[1].action);
    EXPECT_EQ("10.8.0.6", rs[1].to_ip);
    EXPECT_EQ(5683u,      rs[1].to_port);
}

TEST(ParseCustomRules, MalformedJsonReportsError) {
    std::string err;
    auto rs = parse_custom_rules("[{not valid}]", &err);
    EXPECT_TRUE(rs.empty());
    EXPECT_FALSE(err.empty());
}

TEST(ParseCustomRules, NonArrayTopLevelReportsError) {
    std::string err;
    auto rs = parse_custom_rules("{\"action\":\"drop\"}", &err);
    EXPECT_TRUE(rs.empty());
    EXPECT_NE(std::string::npos, err.find("must be a JSON array"));
}

/* ─────────────────────── build_nft_ruleset ───────────────────────── */

TEST(BuildNft, AlwaysStartsWithFlush) {
    State s;
    s.tun_dev = "tun0";
    auto r = build_nft_ruleset(s);
    EXPECT_EQ(0u, r.find("flush table inet iot_router"));
}

TEST(BuildNft, EmitsDnatPerForwardPortForBothTcpAndUdp) {
    State s;
    s.tun_dev          = "tun0";
    s.lwm2m_target_ip  = "10.8.0.6";
    s.forward_ports    = {80, 443, 5684};
    auto r = build_nft_ruleset(s);

    // Each port → one TCP rule + one UDP rule, all DNAT'd to the
    // target IP keeping the original dport.
    for (auto p : {80, 443, 5684}) {
        std::string tcp = "iifname \"tun0\" tcp dport " + std::to_string(p)
                          + " dnat to 10.8.0.6:" + std::to_string(p);
        std::string udp = "iifname \"tun0\" udp dport " + std::to_string(p)
                          + " dnat to 10.8.0.6:" + std::to_string(p);
        EXPECT_NE(std::string::npos, r.find(tcp)) << "missing TCP for port " << p;
        EXPECT_NE(std::string::npos, r.find(udp)) << "missing UDP for port " << p;
    }
}

TEST(BuildNft, SkipsDnatWhenTargetIpMissing) {
    State s;
    s.tun_dev       = "tun0";
    s.forward_ports = {80, 443};
    auto r = build_nft_ruleset(s);
    EXPECT_EQ(std::string::npos, r.find("dnat to"));
    // Still has the empty prerouting chain — proves the script parses.
    EXPECT_NE(std::string::npos, r.find("chain prerouting"));
}

TEST(BuildNft, CustomDropRuleAppears) {
    State s;
    s.tun_dev = "tun0";
    CustomRule cr;
    cr.action = "drop";
    cr.proto  = "tcp";
    cr.dport  = 23;
    s.custom.push_back(cr);
    auto r = build_nft_ruleset(s);
    EXPECT_NE(std::string::npos, r.find("tcp dport 23 drop"));
}

TEST(BuildNft, CustomForwardRuleEmitsDnat) {
    State s;
    s.tun_dev = "tun0";
    CustomRule cr;
    cr.action  = "forward";
    cr.proto   = "udp";
    cr.dport   = 5683;
    cr.to_ip   = "192.168.1.10";
    cr.to_port = 5683;
    s.custom.push_back(cr);
    auto r = build_nft_ruleset(s);
    EXPECT_NE(std::string::npos,
              r.find("udp dport 5683 dnat to 192.168.1.10:5683"));
}

TEST(BuildNft, TunToTunReturnRuleEmittedWhenTunDevPresent) {
    State s;
    s.tun_dev = "tun0";
    auto r = build_nft_ruleset(s);
    EXPECT_NE(std::string::npos,
              r.find("iifname \"tun0\" oifname \"tun0\" return"));
}
