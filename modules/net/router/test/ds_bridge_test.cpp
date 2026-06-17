/// Light-touch DsBridge unit tests — only the don't-need-a-live-
/// ds-server paths. End-to-end watch + writeback is exercised by
/// the integration smoke (`log/L13/net-router-smoke.sh`, D6).

#include <gtest/gtest.h>

#include "ds_bridge.hpp"

using net_router::DsBridge;

namespace {
constexpr const char* kBogus = "/var/run/iot/net-router-no-such-socket.sock";
}

TEST(DsBridge, ConstructAgainstMissingSocketIsNotFatal) {
    DsBridge ds(kBogus);
    EXPECT_FALSE(ds.connected());
    EXPECT_EQ(std::string(kBogus), ds.socket_path());
}

TEST(DsBridge, AccessorsReturnNulloptWhenDisconnected) {
    DsBridge ds(kBogus);
    EXPECT_FALSE(ds.tun_dev().has_value());
    EXPECT_FALSE(ds.lwm2m_target_ip().has_value());
    EXPECT_FALSE(ds.lwm2m_target_port().has_value());
    EXPECT_FALSE(ds.forward_ports().has_value());
    EXPECT_FALSE(ds.custom_rules().has_value());
}

TEST(DsBridge, SettersAreNoopWhenDisconnected) {
    DsBridge ds(kBogus);
    ds.set_state("monitoring");
    ds.set_tun_ip("10.8.0.6");
    ds.set_rules_applied_count(7);
    ds.set_last_apply_unix(1701420000);
    SUCCEED();
}

TEST(DsBridge, OnChangeAcceptsNullCallbackForReset) {
    DsBridge ds(kBogus);
    ds.on_change([](DsBridge::Key) {});
    ds.on_change(nullptr);
    SUCCEED();
}
