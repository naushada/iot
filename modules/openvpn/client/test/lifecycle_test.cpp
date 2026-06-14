/// Pure-logic tests for Lifecycle (mgmt::Event → vpn.* writes).
/// Sinks capture into a struct so we don't need DsBridge / ds-server.

#include <gtest/gtest.h>

#include <string>

#include "lifecycle.hpp"
#include "mgmt_protocol.hpp"

using openvpn_client::Lifecycle;
using openvpn_client::mgmt::Event;
using openvpn_client::mgmt::Parser;

namespace {

/// Records every sink call so a test can assert on the full
/// observed write stream.
struct Spy {
    std::string                state;
    std::string                ip, gateway, netmask, dns;
    bool                       first_push_seen = false;
    int                        first_push_count = 0;
};

Lifecycle::Sinks sinks_for(Spy& s) {
    Lifecycle::Sinks out;
    out.set_state            = [&s](const std::string& v) { s.state   = v; };
    out.set_assigned_ip      = [&s](const std::string& v) { s.ip      = v; };
    out.set_assigned_gateway = [&s](const std::string& v) { s.gateway = v; };
    out.set_assigned_netmask = [&s](const std::string& v) { s.netmask = v; };
    out.set_assigned_dns     = [&s](const std::string& v) { s.dns     = v; };
    out.on_first_push_reply  = [&s]() {
        s.first_push_seen = true;
        s.first_push_count++;
    };
    return out;
}

} // namespace

TEST(Lifecycle, StateConnectingNormalised) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">STATE:0,CONNECTING,,,,,\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("connecting", s.state);
}

TEST(Lifecycle, StateConnectedNormalisedAndAssignedIpRecorded) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">STATE:0,CONNECTED,SUCCESS,10.8.0.6,vpn.example.com,1194,,\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("connected", s.state);
    EXPECT_EQ("10.8.0.6",  s.ip);
}

TEST(Lifecycle, AssignIpCarriesIpEvenBeforeConnected) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">STATE:0,ASSIGN_IP,,10.8.0.6,,,,,\n");
    while (auto ev = p.next()) l.step(*ev);
    // ASSIGN_IP maps to connecting per normalise_state; ip filled.
    EXPECT_EQ("connecting", s.state);
    EXPECT_EQ("10.8.0.6",   s.ip);
}

TEST(Lifecycle, PushReplyFillsIfconfigGatewayAndDns) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,"
                     "ifconfig 10.8.0.6 255.255.255.0\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("10.8.0.6",      s.ip);
    EXPECT_EQ("255.255.255.0", s.netmask);
    EXPECT_EQ("10.8.0.1",      s.gateway);
    EXPECT_EQ("1.1.1.1",       s.dns);
    EXPECT_TRUE(s.first_push_seen);
}

TEST(Lifecycle, LogPushReplyFillsIfconfigGatewayAndDns) {
    // Real openvpn delivers the pushed config via the LOG, not a >PUSH_REPLY
    // mgmt event: ">LOG:<time>,<flags>,PUSH: Received control message:
    // 'PUSH_REPLY,...'". The lifecycle must parse it (requires `log on`).
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">LOG:1718,I,PUSH: Received control message: "
                     "'PUSH_REPLY,route-gateway 10.9.0.1,topology subnet,"
                     "ifconfig 10.9.0.2 255.255.255.0,dhcp-option DNS 1.1.1.1,"
                     "peer-id 0'\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("10.9.0.2",      s.ip);
    EXPECT_EQ("255.255.255.0", s.netmask);
    EXPECT_EQ("10.9.0.1",      s.gateway);
    EXPECT_EQ("1.1.1.1",       s.dns);
    EXPECT_TRUE(s.first_push_seen);
}

TEST(Lifecycle, LogWithoutPushReplyIgnored) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">LOG:1718,I,OpenVPN 2.6 starting\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("", s.gateway);
    EXPECT_FALSE(s.first_push_seen);
}

TEST(Lifecycle, PushReplyJoinsMultipleDnsCommaSeparated) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p; p.feed(">PUSH_REPLY:dhcp-option DNS 1.1.1.1,dhcp-option DNS 8.8.8.8\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("1.1.1.1,8.8.8.8", s.dns);
}

TEST(Lifecycle, OnFirstPushReplyFiresOnce) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p;
    p.feed(">PUSH_REPLY:ifconfig 10.8.0.6 255.255.255.0\n");
    p.feed(">PUSH_REPLY:ifconfig 10.8.0.7 255.255.255.0\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ(1, s.first_push_count);
    EXPECT_TRUE(l.saw_push_reply());
}

TEST(Lifecycle, RealisticStreamYieldsExpectedFinalState) {
    Spy s;
    Lifecycle l(sinks_for(s));
    Parser p;
    p.feed(
        ">INFO:OpenVPN Management Interface Version 1\n"
        ">STATE:1,CONNECTING,,,,,\n"
        ">STATE:2,WAIT,,,,,\n"
        ">STATE:3,AUTH,,,,,\n"
        ">STATE:4,GET_CONFIG,,,,,\n"
        ">PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,"
        "ifconfig 10.8.0.6 255.255.255.0\n"
        ">STATE:5,ASSIGN_IP,,10.8.0.6,,,,,\n"
        ">STATE:6,CONNECTED,SUCCESS,10.8.0.6,vpn.example.com,1194,,\n");
    while (auto ev = p.next()) l.step(*ev);
    EXPECT_EQ("connected",     s.state);
    EXPECT_EQ("10.8.0.6",      s.ip);
    EXPECT_EQ("255.255.255.0", s.netmask);
    EXPECT_EQ("10.8.0.1",      s.gateway);
    EXPECT_EQ("1.1.1.1",       s.dns);
    EXPECT_TRUE(s.first_push_seen);
}
