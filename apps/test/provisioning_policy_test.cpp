/// PSK provisioning (tasks E/K/G) — pure policy tests.

#include <gtest/gtest.h>

#include "provisioning_policy.hpp"

using iot::resolve_endpoint;

TEST(EndpointResolution, CliOverrideWins) {
    auto r = resolve_endpoint("urn:dev:cli", std::string("SER123"),
                              /*is_rpi*/true, "DETECT");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("urn:dev:cli", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);   // never auto-writes on override
}

TEST(EndpointResolution, DataStoreSerialUsedWhenNoCli) {
    auto r = resolve_endpoint("", std::string("SER123"), true, "DETECT");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("SER123", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);   // already persisted
}

TEST(EndpointResolution, RpiAutoFillWhenSerialEmpty) {
    auto r = resolve_endpoint("", std::nullopt, /*is_rpi*/true, "100000abcd");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("100000abcd", r.endpoint);
    EXPECT_EQ("100000abcd", r.serial_to_write);  // signal caller to persist
}

TEST(EndpointResolution, RpiAutoFillWhenSerialBlank) {
    // ds_serial present but empty string behaves like "not set".
    auto r = resolve_endpoint("", std::string(""), true, "100000abcd");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("100000abcd", r.serial_to_write);
}

TEST(EndpointResolution, NonRpiNoSerialDefers) {
    auto r = resolve_endpoint("", std::nullopt, /*is_rpi*/false, "");
    EXPECT_FALSE(r.ready);
    EXPECT_EQ("", r.endpoint);
    EXPECT_EQ("", r.serial_to_write);
}

TEST(EndpointResolution, NonRpiInstallerSerialUsed) {
    auto r = resolve_endpoint("", std::string("INSTALLER-SN"), false, "");
    EXPECT_TRUE(r.ready);
    EXPECT_EQ("INSTALLER-SN", r.endpoint);
}

TEST(RestartOnPskChange, NoRestartBeforeInit) {
    EXPECT_FALSE(iot::should_restart_on_psk_change(
        /*initialized*/false, "old", "new"));
}

TEST(RestartOnPskChange, NoRestartOnSelfWrite) {
    // loaded == observed → the value we just wrote; no restart.
    EXPECT_FALSE(iot::should_restart_on_psk_change(true, "abcd", "abcd"));
}

TEST(RestartOnPskChange, RestartOnGenuineChange) {
    EXPECT_TRUE(iot::should_restart_on_psk_change(true, "abcd", "ef01"));
}

TEST(CoapClientError, ClassifiesByUpperBits) {
    EXPECT_TRUE(iot::is_coap_client_error(0x83));   // 4.03 Forbidden
    EXPECT_TRUE(iot::is_coap_client_error(0x80));   // 4.00 Bad Request
    EXPECT_FALSE(iot::is_coap_client_error(0x41));  // 2.01 Created
    EXPECT_FALSE(iot::is_coap_client_error(0xA0));  // 5.00 Server Error
}

TEST(Rebootstrap, TriggersOnDtlsFailure) {
    EXPECT_TRUE(iot::should_rebootstrap(/*dtls*/true, /*reg*/false));
}

TEST(Rebootstrap, TriggersOnRegistrationReject) {
    EXPECT_TRUE(iot::should_rebootstrap(false, true));
}

TEST(Rebootstrap, NoTriggerWhenHealthy) {
    EXPECT_FALSE(iot::should_rebootstrap(false, false));
}
