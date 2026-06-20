/// Unit tests for the OpenVPN server config renderer (pure).

#include <gtest/gtest.h>

#include "openvpn_server.hpp"

using server::openvpn::OpenVpnServerConfig;
using server::openvpn::build_server_config;
using server::openvpn::cidr_to_net_mask;

namespace {

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

}  // namespace
