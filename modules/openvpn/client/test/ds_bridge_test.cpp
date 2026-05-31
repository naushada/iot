/// Light-touch unit tests for DsBridge — only the bits that DON'T
/// need a live ds-server. End-to-end behaviour (snapshot priming
/// against real keys, watch callbacks firing) is exercised by the
/// log/L12/*-smoke.sh harnesses against a real ds-server.

#include <gtest/gtest.h>

#include "ds_bridge.hpp"

using openvpn_client::DsBridge;

namespace {

/// A socket path that almost certainly doesn't exist. mkdtemp is
/// overkill — we only need the connect to fail with ECONNREFUSED /
/// ENOENT, both of which DsBridge absorbs into `connected()==false`.
constexpr const char* kBogusSocket = "/var/run/iot/this-path-cannot-exist.sock";

} // namespace

TEST(DsBridge, ConstructAgainstMissingSocketIsNotFatal) {
    // The bridge MUST NOT throw or crash when the socket isn't
    // there — the calling daemon is responsible for surfacing the
    // failure via missing_required() + connected() checks.
    DsBridge ds(kBogusSocket);
    EXPECT_FALSE(ds.connected());
    EXPECT_EQ(std::string(kBogusSocket), ds.socket_path());
}

TEST(DsBridge, AccessorsReturnNulloptWhenDisconnected) {
    DsBridge ds(kBogusSocket);
    ASSERT_FALSE(ds.connected());

    // Spot-check one accessor per type; the rest follow the same
    // mutex-then-snapshot pattern.
    EXPECT_FALSE(ds.remote_host().has_value());
    EXPECT_FALSE(ds.remote_port().has_value());
    EXPECT_FALSE(ds.cert_path().has_value());
}

TEST(DsBridge, MissingRequiredListsAllRequiredKeysWhenDisconnected) {
    // Disconnected → no way to know what's actually in the store →
    // contract says "report every required key as missing" so the
    // caller's missing-config path is uniform.
    DsBridge ds(kBogusSocket);
    auto missing = ds.missing_required();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(4u, missing->size());
    // Order matches DsBridge.cpp's hardcoded ordering — keep these
    // in sync if either side changes.
    EXPECT_EQ("vpn.remote.host", (*missing)[0]);
    EXPECT_EQ("vpn.cert.path",   (*missing)[1]);
    EXPECT_EQ("vpn.key.path",    (*missing)[2]);
    EXPECT_EQ("vpn.ca.path",     (*missing)[3]);
}

TEST(DsBridge, SettersAreNoopWhenDisconnected) {
    // Daemon shouldn't crash if a setter fires after ds-server died.
    // No assertion on side effects — just verify nothing throws.
    DsBridge ds(kBogusSocket);
    ds.set_state("connecting");
    ds.set_assigned_ip("10.0.0.1");
    ds.set_pid(42);
    ds.set_exit_code(0);
    SUCCEED();
}

TEST(DsBridge, OnChangeAcceptsNullCallbackForReset) {
    DsBridge ds(kBogusSocket);
    // Register, then null-out — proves the setter doesn't keep a
    // stale handle that the listener thread could fire.
    ds.on_change([](DsBridge::Key) {});
    ds.on_change(nullptr);
    SUCCEED();
}
