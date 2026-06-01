/// Light-touch unit tests for wifi-client's DsBridge — only the bits
/// that DON'T need a live ds-server. End-to-end prime + watch
/// behaviour against real keys lives in log/L15/smoke.sh against a
/// real ds-server (D8). Same trade-off openvpn-client took.

#include <gtest/gtest.h>

#include "ds_bridge.hpp"

using wifi_client::DsBridge;

namespace {

/// A socket path that almost certainly doesn't exist. The bridge
/// must absorb the failure as `connected()==false` without crashing.
constexpr const char* kBogusSocket =
    "/var/run/iot/this-wifi-path-cannot-exist.sock";

} // namespace

// ─────────────────────────── REQ-WIFI-007 ───────────────────────────
// "DsBridge MUST prime by get-ing every read key, then register for
// change events." The prime+watch path requires a live ds-server;
// the smoke covers it. Here we verify the disconnected branch is
// non-crashing so the supervisor's "ds down" diagnostic is uniform.

TEST(WIFI_REQ_WIFI_007_start_primes_then_registers,
     construct_against_missing_socket_is_not_fatal) {
    DsBridge ds(kBogusSocket);
    EXPECT_FALSE(ds.connected());
    EXPECT_EQ(std::string(kBogusSocket), ds.socket_path());
}

TEST(WIFI_REQ_WIFI_007_start_primes_then_registers,
     accessors_return_nullopt_when_disconnected) {
    DsBridge ds(kBogusSocket);
    ASSERT_FALSE(ds.connected());
    // Spot-check one accessor per type; the rest follow the same
    // mutex-then-snapshot pattern.
    EXPECT_FALSE(ds.iface().has_value());
    EXPECT_FALSE(ds.scan_max_results().has_value());
    EXPECT_FALSE(ds.networks().has_value());
}

TEST(WIFI_REQ_WIFI_007_start_primes_then_registers,
     missing_required_lists_placeholder_when_disconnected) {
    // Disconnected → surface a placeholder list so the caller's
    // "ds unreachable" branch is uniform with openvpn-client's
    // missing_required() shape. Schema defaults make connected()==true
    // → nullopt the normal case (no genuinely missing keys at this
    // layer; JSON validation of wifi.networks is the Supervisor's job).
    DsBridge ds(kBogusSocket);
    auto missing = ds.missing_required();
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->empty());
    // Order matches DsBridge.cpp; keep in sync if either side changes.
    EXPECT_EQ("wifi.iface", (*missing)[0]);
}

// ─────────────────────────── REQ-WIFI-008 ───────────────────────────
// Setters must log + return without throwing on wire failure. We
// can't directly inspect log output here; the assertion is "no
// throw, no crash" when fired against a disconnected bridge.

TEST(WIFI_REQ_WIFI_008_setter_logs_on_failure_no_throw,
     all_setters_noop_when_disconnected) {
    DsBridge ds(kBogusSocket);
    ASSERT_FALSE(ds.connected());

    // Every published write key. If a future setter throws on a
    // disconnected bridge, this test catches it.
    ds.set_assoc_state("scanning");
    ds.set_assoc_ssid("HomeAP");
    ds.set_assoc_bssid("aa:bb:cc:dd:ee:ff");
    ds.set_signal_rssi(-52);
    ds.set_scan_results("[]");
    ds.set_scan_last_unix(1717293600u);
    ds.set_dhcp_state("idle");
    ds.set_dhcp_ip("10.0.0.42");
    ds.set_pid_wpa(0u);
    ds.set_pid_dhcp(0u);
    ds.set_last_error("");
    SUCCEED();
}

TEST(WIFI_REQ_WIFI_008_setter_logs_on_failure_no_throw,
     negative_rssi_does_not_underflow_or_throw) {
    // wifi.signal.rssi is a signed int — operator values are dBm,
    // typically -30 to -90. Make sure the setter accepts the range.
    DsBridge ds(kBogusSocket);
    ds.set_signal_rssi(-90);
    ds.set_signal_rssi(0);
    ds.set_signal_rssi(-30);
    SUCCEED();
}

// ─────────────────────────── REQ-WIFI-009 ───────────────────────────
// on_change(nullptr) MUST clear a previously-registered callback so
// the Supervisor can swap handlers between phases without leaking
// the old one onto the listener thread.

TEST(WIFI_REQ_WIFI_009_on_change_null_callback_resets,
     on_change_accepts_null_after_real_callback) {
    DsBridge ds(kBogusSocket);
    bool fired = false;
    ds.on_change([&](DsBridge::Key) { fired = true; });
    ds.on_change(nullptr);
    // We can't drive the listener thread from a disconnected bridge,
    // but the SetUp/TearDown alone proves the registration setter
    // doesn't crash when called with nullptr after a real callback.
    EXPECT_FALSE(fired);
}

TEST(WIFI_REQ_WIFI_009_on_change_null_callback_resets,
     on_change_idempotent_under_repeated_null) {
    DsBridge ds(kBogusSocket);
    ds.on_change(nullptr);
    ds.on_change(nullptr);
    ds.on_change(nullptr);
    SUCCEED();
}
