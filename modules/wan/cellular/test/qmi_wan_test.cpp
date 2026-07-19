#include "qmi_wan.hpp"

#include <gtest/gtest.h>

using namespace cellular;

// ── mask_to_prefix ──────────────────────────────────────────────────────────

TEST(MaskToPrefix, CommonMasks) {
    EXPECT_EQ(mask_to_prefix("255.255.255.252"), 30);
    EXPECT_EQ(mask_to_prefix("255.255.255.255"), 32);
    EXPECT_EQ(mask_to_prefix("255.255.255.0"),   24);
    EXPECT_EQ(mask_to_prefix("255.255.0.0"),     16);
    EXPECT_EQ(mask_to_prefix("0.0.0.0"),          0);
}

TEST(MaskToPrefix, RejectsGarbage) {
    EXPECT_EQ(mask_to_prefix(""),               0);
    EXPECT_EQ(mask_to_prefix("not.a.mask"),     0);
    EXPECT_EQ(mask_to_prefix("255.0.255.0"),    0);   // non-contiguous
    EXPECT_EQ(mask_to_prefix("256.255.255.0"),  0);   // octet out of range
}

// ── parse_current_settings ──────────────────────────────────────────────────

TEST(CurrentSettings, FullOutput) {
    const std::string out =
        "[/dev/cdc-wdm0] Current settings retrieved:\n"
        "           IP Family: IPv4\n"
        "        IPv4 address: 100.115.65.218\n"
        "    IPv4 subnet mask: 255.255.255.252\n"
        "IPv4 gateway address: 100.115.65.217\n"
        "    IPv4 primary DNS: 117.96.122.74\n"
        "  IPv4 secondary DNS: 59.144.127.117\n"
        "                 MTU: 1500\n"
        "             Domains: none\n";
    const auto s = parse_current_settings(out);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.ip,          "100.115.65.218");
    EXPECT_EQ(s.gateway,     "100.115.65.217");
    EXPECT_EQ(s.subnet_mask, "255.255.255.252");
    EXPECT_EQ(s.prefix,      30);
    ASSERT_EQ(s.dns.size(),  2u);
    EXPECT_EQ(s.dns[0],      "117.96.122.74");
    EXPECT_EQ(s.dns[1],      "59.144.127.117");
    EXPECT_EQ(s.mtu,         1500);
}

TEST(CurrentSettings, GatewayNotMistakenForAddress) {
    // "IPv4 gateway address" must not be captured by the "IPv4 address" lookup.
    const std::string out =
        "        IPv4 address: 10.0.0.2\n"
        "IPv4 gateway address: 10.0.0.1\n";
    const auto s = parse_current_settings(out);
    EXPECT_EQ(s.ip,      "10.0.0.2");
    EXPECT_EQ(s.gateway, "10.0.0.1");
}

TEST(CurrentSettings, OutOfCallIsInvalid) {
    const std::string out =
        "error: couldn't get current settings: "
        "QMI protocol error (15): 'OutOfCall'\n";
    const auto s = parse_current_settings(out);
    EXPECT_FALSE(s.valid);
    EXPECT_TRUE(s.ip.empty());
}

TEST(CurrentSettings, SingleDns) {
    const std::string out =
        "        IPv4 address: 100.84.33.32\n"
        "    IPv4 primary DNS: 117.96.122.74\n"
        "                 MTU: 1500\n";
    const auto s = parse_current_settings(out);
    ASSERT_EQ(s.dns.size(), 1u);
    EXPECT_EQ(s.dns[0], "117.96.122.74");
}

// ── parse_start_network ─────────────────────────────────────────────────────

TEST(StartNetwork, Success) {
    const std::string out =
        "[/dev/cdc-wdm0] Network started\n"
        "\tPacket data handle: '2235671520'\n"
        "[/dev/cdc-wdm0] Client ID not released:\n"
        "\tService: 'wds'\n"
        "\t    CID: '10'\n";
    const auto r = parse_start_network(out);
    EXPECT_EQ(r.status, StartResult::Status::Started);
    EXPECT_EQ(r.handle, "2235671520");
}

TEST(StartNetwork, CidTimeout) {
    const std::string out =
        "error: couldn't create client for the 'wds' service: "
        "CID allocation failed in the CTL client: Transaction timed out\n";
    const auto r = parse_start_network(out);
    EXPECT_EQ(r.status, StartResult::Status::CidTimeout);
}

TEST(StartNetwork, CallFailedCarriesEndReason) {
    const std::string out =
        "error: couldn't start network: QMI protocol error (14): 'CallFailed'\n"
        "call end reason (3): generic-no-service\n"
        "verbose call end reason (3,1075): [cm] (null)\n"
        "[/dev/cdc-wdm0] Client ID not released:\n"
        "\t    CID: '10'\n";
    const auto r = parse_start_network(out);
    EXPECT_EQ(r.status,     StartResult::Status::CallFailed);
    EXPECT_EQ(r.end_reason, "generic-no-service");
    EXPECT_EQ(r.verbose,    "[cm] (null)");
}

TEST(StartNetwork, EmptyIsUnknown) {
    EXPECT_EQ(parse_start_network("").status, StartResult::Status::Unknown);
}

// ── parse_ps_attached ───────────────────────────────────────────────────────

TEST(PsAttached, Attached) {
    const std::string out =
        "[/dev/cdc-wdm0] Successfully got serving system:\n"
        "\tRegistration state: 'registered'\n"
        "\tCS: 'attached'\n"
        "\tPS: 'attached'\n"
        "\tSelected network: '3gpp'\n";
    EXPECT_TRUE(parse_ps_attached(out));
}

TEST(PsAttached, Detached) {
    const std::string out =
        "\tRegistration state: 'registered'\n"
        "\tCS: 'attached'\n"
        "\tPS: 'detached'\n";
    EXPECT_FALSE(parse_ps_attached(out));
}

TEST(PsAttached, MissingIsFalse) {
    EXPECT_FALSE(parse_ps_attached("\tRegistration state: 'registered'\n"));
}

// ── parse_connected ─────────────────────────────────────────────────────────

TEST(Connected, Connected) {
    EXPECT_TRUE(parse_connected(
        "[/dev/cdc-wdm0] Connection status: 'connected'\n"));
}

TEST(Connected, Disconnected) {
    EXPECT_FALSE(parse_connected(
        "[/dev/cdc-wdm0] Connection status: 'disconnected'\n"));
}
