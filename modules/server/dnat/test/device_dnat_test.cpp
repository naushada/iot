/// Table-driven tests for the pure per-device DNAT ruleset generator.

#include <gtest/gtest.h>

#include <string>

#include "device_dnat.hpp"

using server::dnat::build_device_dnat_ruleset;
using server::dnat::DeviceForward;
using server::dnat::format_mapping;
using server::dnat::RulesetInput;

namespace {
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

TEST(DeviceDnat, EmptyStillEmitsScopedTableAndFlush) {
    RulesetInput in;  // no devices
    auto s = build_device_dnat_ruleset(in);
    // `add table` MUST precede `flush table` so a fresh netns (no table yet)
    // doesn't abort the whole apply on a "flush: No such file" error.
    EXPECT_TRUE(contains(s, "add table ip iot_cloud_dnat"));
    EXPECT_TRUE(contains(s, "flush table ip iot_cloud_dnat"));
    EXPECT_LT(s.find("add table ip iot_cloud_dnat"),
              s.find("flush table ip iot_cloud_dnat"));
    EXPECT_TRUE(contains(s, "table ip iot_cloud_dnat {"));
    EXPECT_TRUE(contains(s, "type nat hook prerouting"));
    EXPECT_TRUE(contains(s, "type nat hook postrouting"));
}

TEST(DeviceDnat, SingleDeviceDnatRule) {
    RulesetInput in;
    in.ui_port = 80;
    in.devices = {{"10.9.0.12", 5001}};
    auto s = build_device_dnat_ruleset(in);
    EXPECT_TRUE(contains(s, "tcp dport 5001 dnat to 10.9.0.12:80"));
}

TEST(DeviceDnat, MultipleDevicesEachGetARule) {
    RulesetInput in;
    in.ui_port = 80;
    in.devices = {{"10.9.0.12", 5001}, {"10.9.0.13", 5002}};
    auto s = build_device_dnat_ruleset(in);
    EXPECT_TRUE(contains(s, "tcp dport 5001 dnat to 10.9.0.12:80"));
    EXPECT_TRUE(contains(s, "tcp dport 5002 dnat to 10.9.0.13:80"));
}

TEST(DeviceDnat, NonDefaultUiPortHonored) {
    RulesetInput in;
    in.ui_port = 443;
    in.devices = {{"10.9.0.12", 5001}};
    auto s = build_device_dnat_ruleset(in);
    EXPECT_TRUE(contains(s, "tcp dport 5001 dnat to 10.9.0.12:443"));
}

TEST(DeviceDnat, SkipsDevicesMissingTunIpOrPort) {
    RulesetInput in;
    in.devices = {{"", 5001}, {"10.9.0.13", 0}, {"10.9.0.14", 5003}};
    auto s = build_device_dnat_ruleset(in);
    EXPECT_FALSE(contains(s, "dport 5001"));
    EXPECT_FALSE(contains(s, "10.9.0.13"));
    EXPECT_TRUE(contains(s, "tcp dport 5003 dnat to 10.9.0.14:80"));
}

TEST(DeviceDnat, MasqueradeAndForwardScopedToTunDev) {
    RulesetInput in;
    in.tun_dev = "tun0";
    in.devices = {{"10.9.0.12", 5001}};
    auto s = build_device_dnat_ruleset(in);
    EXPECT_TRUE(contains(s, "oifname \"tun0\" masquerade"));
    EXPECT_TRUE(contains(s, "oifname \"tun0\" accept"));
    EXPECT_TRUE(contains(s, "iifname \"tun0\" accept"));
}

TEST(DeviceDnat, FormatMappingShape) {
    EXPECT_EQ("tcp dport 5001 -> 10.9.0.12:80",
              format_mapping(DeviceForward{"10.9.0.12", 5001}, 80));
}
