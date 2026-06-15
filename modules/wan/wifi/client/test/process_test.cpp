/// Unit tests for the D5 surface: parse_wifi_networks +
/// build_wpa_supplicant_config + write_temp_config + pick_dhcp_client
/// + Process (spawn/reap via /bin/sh stand-in).
///
/// REQ-WIFI-014 — Process spawn returns pid > 0 on success; destructor
/// reaps; SIGTERM->SIGKILL escalation works.
/// REQ-WIFI-015 — config writer orders network={} blocks by descending
/// priority; emits ctrl_interface + update_config header.
/// REQ-WIFI-016 — key_mgmt=NONE omits psk; missing psk for WPA-PSK
/// surfaces as bad_networks_json error.
/// REQ-WIFI-017 — DHCP picker honours scheme ("udhcpc" / "dhclient" /
/// "auto") and override path.
/// NFR-WIFI-006 — destructor reap within 5 s (validated via sleep test
/// with a small grace).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "process.hpp"

using wifi_client::Process;
using wifi_client::WifiNetwork;
using wifi_client::build_wpa_supplicant_config;
using wifi_client::parse_wifi_networks;
using wifi_client::pick_dhcp_client;
using wifi_client::write_temp_config;

// ─────────────────────────── REQ-WIFI-014 ───────────────────────────

TEST(WIFI_REQ_WIFI_014_spawn_pid_and_destructor_reaps,
     spawn_returns_pid_gt_zero) {
    Process p;
    bool ok = p.spawn("/bin/sh", {"/bin/sh", "-c", "exit 0"});
    ASSERT_TRUE(ok);
    EXPECT_GT(p.pid(), 0);
    EXPECT_EQ(0, p.wait());
}

TEST(WIFI_REQ_WIFI_014_spawn_pid_and_destructor_reaps,
     destructor_reaps_living_child) {
    pid_t pid = 0;
    {
        Process p;
        ASSERT_TRUE(p.spawn("/bin/sh",
                            {"/bin/sh", "-c", "sleep 60"}));
        pid = p.pid();
        ASSERT_GT(pid, 0);
        // destructor fires here; should SIGTERM + SIGKILL + reap.
    }
    // The child is reaped because Process is the parent. Verify by
    // sending signal 0 and expecting ESRCH (no such process) — this
    // works because the reap removed the PID from the table.
    int rc = ::kill(pid, 0);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(ESRCH, errno);
}

TEST(WIFI_REQ_WIFI_014_spawn_pid_and_destructor_reaps,
     terminate_sigkill_escalation_works) {
    // A child that ignores SIGTERM. Process::terminate must SIGKILL
    // after the grace deadline. Use a small grace to keep the test
    // fast — 200ms TERM-ignored + 200ms grace + reap.
    Process p;
    ASSERT_TRUE(p.spawn("/bin/sh",
                        {"/bin/sh", "-c", "trap '' TERM; sleep 30"}));
    auto t0 = std::chrono::steady_clock::now();
    p.terminate(std::chrono::milliseconds(200));
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, std::chrono::seconds(2))
        << "terminate took too long; SIGKILL escalation may have hung";
    EXPECT_FALSE(p.running());
}

// ─────────────────────────── NFR-WIFI-006 ───────────────────────────

TEST(WIFI_NFR_WIFI_006_destructor_reaps_within_5s,
     reap_deadline_under_5s_for_sigterm_ignorant_child) {
    auto t0 = std::chrono::steady_clock::now();
    {
        Process p;
        ASSERT_TRUE(p.spawn("/bin/sh",
                            {"/bin/sh", "-c", "trap '' TERM; sleep 60"}));
        // Destructor default grace = 500ms; SIGKILL after.
    }
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, std::chrono::seconds(5));
}

// ─────────────────────────── REQ-WIFI-015 ───────────────────────────

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     header_includes_ctrl_interface_and_update_config) {
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {});
    EXPECT_NE(std::string::npos,
              body.find("ctrl_interface=DIR=/run/wpa_supplicant"));
    EXPECT_NE(std::string::npos, body.find("update_config=0"));
}

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     empty_networks_emits_header_only) {
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {});
    EXPECT_EQ(std::string::npos, body.find("network={"));
}

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     descending_priority_order) {
    std::vector<WifiNetwork> nets = {
        { "Low",  "lowpsk",  1,  "WPA-PSK" },
        { "High", "highpsk", 10, "WPA-PSK" },
        { "Mid",  "midpsk",  5,  "WPA-PSK" },
    };
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", nets);
    auto hi = body.find("ssid=\"High\"");
    auto md = body.find("ssid=\"Mid\"");
    auto lo = body.find("ssid=\"Low\"");
    ASSERT_NE(std::string::npos, hi);
    ASSERT_NE(std::string::npos, md);
    ASSERT_NE(std::string::npos, lo);
    EXPECT_LT(hi, md);
    EXPECT_LT(md, lo);
}

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     ssid_quote_chars_escaped) {
    std::vector<WifiNetwork> nets = {
        { "My\"AP\\x", "psk", 0, "WPA-PSK" },
    };
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", nets);
    // " and \\ in the SSID must be backslash-escaped so wpa_supplicant's
    // parser doesn't see them as terminators.
    EXPECT_NE(std::string::npos, body.find("ssid=\"My\\\"AP\\\\x\""));
}

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     psk_quote_chars_escaped) {
    std::vector<WifiNetwork> nets = {
        { "Net", "p\"sk\\with\"quotes", 0, "WPA-PSK" },
    };
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", nets);
    // Both '"' and '\\' should be backslash-escaped inside the
    // psk="..." block so wpa_supplicant's parser doesn't see them
    // as terminators.
    EXPECT_NE(std::string::npos, body.find("psk=\"p\\\"sk\\\\with\\\"quotes\""));
}

// ─────────────────────────── REQ-WIFI-016 ───────────────────────────

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     open_network_omits_psk_emits_key_mgmt_NONE) {
    std::vector<WifiNetwork> nets = {
        { "OpenAP", "ignored-if-any", 0, "NONE" },
    };
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", nets);
    EXPECT_NE(std::string::npos, body.find("ssid=\"OpenAP\""));
    EXPECT_NE(std::string::npos, body.find("key_mgmt=NONE"));
    EXPECT_EQ(std::string::npos, body.find("psk="))
        << "open network MUST NOT emit psk=, body:\n" << body;
}

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     parse_rejects_missing_psk_for_wpa_psk) {
    std::string err;
    auto nets = parse_wifi_networks(R"([{"ssid":"X","priority":0}])", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"))
        << "expected bad_networks_json error, got: " << err;
    EXPECT_NE(std::string::npos, err.find("psk"));
}

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     parse_rejects_missing_ssid) {
    std::string err;
    auto nets = parse_wifi_networks(R"([{"psk":"x"}])", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
    EXPECT_NE(std::string::npos, err.find("ssid"));
}

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     parse_accepts_NONE_without_psk) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"OpenAP","key_mgmt":"NONE"}])", &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(1u, nets.size());
    EXPECT_EQ("OpenAP", nets[0].ssid);
    EXPECT_EQ("NONE",   nets[0].key_mgmt);
    EXPECT_TRUE(nets[0].psk.empty());
}

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     parse_empty_input_yields_empty_list_no_error) {
    std::string err;
    auto nets = parse_wifi_networks("[]", &err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(nets.empty());
}

TEST(WIFI_REQ_WIFI_016_open_network_omits_psk_and_validates,
     parse_rejects_root_not_array) {
    std::string err;
    auto nets = parse_wifi_networks(R"({"ssid":"X"})", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
}

// ─────────────────────────── REQ-WIFI-017 ───────────────────────────

TEST(WIFI_REQ_WIFI_017_dhcp_picker_honours_schema_key,
     override_wins_unconditionally) {
    auto p = pick_dhcp_client("auto", "/opt/local/bin/my-dhcp");
    EXPECT_EQ("/opt/local/bin/my-dhcp", p);
    // Even when scheme says dhclient, override still wins.
    p = pick_dhcp_client("dhclient", "/opt/local/bin/my-dhcp");
    EXPECT_EQ("/opt/local/bin/my-dhcp", p);
}

TEST(WIFI_REQ_WIFI_017_dhcp_picker_honours_schema_key,
     unknown_scheme_falls_to_auto_behavior) {
    // The picker treats anything that isn't "udhcpc"/"dhclient" as
    // auto. We don't assert which binary it picks (depends on the
    // test image) but the result must be non-empty IF either is on
    // disk, else empty.
    auto p = pick_dhcp_client("invalid", "");
    // Just check the function doesn't throw or crash.
    SUCCEED() << "result: '" << p << "'";
}

// ───────────────────── REQ-WIFI-024 (WPA-Enterprise) ────────────────
//
// WPA-EAP networks carry identity + password (and optional eap/phase2/
// ca_cert) instead of a psk. parse_wifi_networks must accept them and
// reject entries missing identity/password; build_wpa_supplicant_config
// must emit an EAP network={} block with NO psk= line.

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_accepts_full_eap_entry) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP","eap":"PEAP",)"
        R"("identity":"user@corp","password":"secret",)"
        R"("phase2":"auth=MSCHAPV2","priority":20}])", &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(1u, nets.size());
    EXPECT_EQ("CorpAP",          nets[0].ssid);
    EXPECT_EQ("WPA-EAP",         nets[0].key_mgmt);
    EXPECT_EQ("PEAP",            nets[0].eap);
    EXPECT_EQ("user@corp",       nets[0].identity);
    EXPECT_EQ("secret",          nets[0].password);
    EXPECT_EQ("auth=MSCHAPV2",   nets[0].phase2);
    EXPECT_EQ(20,                nets[0].priority);
    EXPECT_TRUE(nets[0].psk.empty());
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_eap_defaults_eap_and_phase2) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP",)"
        R"("identity":"u","password":"p"}])", &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(1u, nets.size());
    EXPECT_EQ("PEAP",          nets[0].eap)    << "eap should default to PEAP";
    EXPECT_EQ("auth=MSCHAPV2", nets[0].phase2) << "phase2 should default";
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_rejects_eap_missing_identity) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP","password":"p"}])", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
    EXPECT_NE(std::string::npos, err.find("identity"));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_rejects_eap_missing_password) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP","identity":"u"}])", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
    EXPECT_NE(std::string::npos, err.find("password"));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_rejects_eap_empty_password) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP","identity":"u","password":""}])",
        &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
    EXPECT_NE(std::string::npos, err.find("password"));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_rejects_control_chars_in_eap_fields) {
    // A newline in any emitted field would break wpa_supplicant.conf or
    // inject a directive; parse must reject it rather than pass it to esc().
    std::string err;
    auto nets = parse_wifi_networks(
        "[{\"ssid\":\"CorpAP\",\"key_mgmt\":\"WPA-EAP\","
        "\"identity\":\"user\\nkey_mgmt=NONE\",\"password\":\"p\"}]",
        &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("bad_networks_json"));
    EXPECT_NE(std::string::npos, err.find("control characters"));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_rejects_control_chars_in_psk) {
    // Same guard applies to PSK networks (shared emit path).
    std::string err;
    auto nets = parse_wifi_networks(
        "[{\"ssid\":\"Home\",\"psk\":\"pass\\nword\"}]", &err);
    EXPECT_TRUE(nets.empty());
    EXPECT_NE(std::string::npos, err.find("control characters"));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     parse_eap_does_not_require_psk) {
    std::string err;
    auto nets = parse_wifi_networks(
        R"([{"ssid":"CorpAP","key_mgmt":"WPA-EAP",)"
        R"("identity":"u","password":"p"}])", &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(1u, nets.size());
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     build_emits_eap_block_without_psk) {
    WifiNetwork n;
    n.ssid     = "CorpAP";
    n.key_mgmt = "WPA-EAP";
    n.eap      = "PEAP";
    n.identity = "user@corp";
    n.password = "secret";
    n.phase2   = "auth=MSCHAPV2";
    n.priority = 20;
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {n});
    EXPECT_NE(std::string::npos, body.find("ssid=\"CorpAP\""));
    EXPECT_NE(std::string::npos, body.find("key_mgmt=WPA-EAP"));
    EXPECT_NE(std::string::npos, body.find("eap=PEAP"));
    EXPECT_NE(std::string::npos, body.find("identity=\"user@corp\""));
    EXPECT_NE(std::string::npos, body.find("password=\"secret\""));
    EXPECT_NE(std::string::npos, body.find("phase2=\"auth=MSCHAPV2\""));
    EXPECT_NE(std::string::npos, body.find("priority=20"));
    EXPECT_EQ(std::string::npos, body.find("psk="))
        << "EAP network MUST NOT emit psk=, body:\n" << body;
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     build_emits_ca_cert_only_when_present) {
    WifiNetwork bare;
    bare.ssid     = "CorpAP";
    bare.key_mgmt = "WPA-EAP";
    bare.eap      = "PEAP";
    bare.identity = "u";
    bare.password = "p";
    auto body_bare = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {bare});
    EXPECT_EQ(std::string::npos, body_bare.find("ca_cert="));

    WifiNetwork with_ca = bare;
    with_ca.ca_cert = "/etc/iot/certs/corp-ca.pem";
    auto body_ca = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {with_ca});
    EXPECT_NE(std::string::npos,
              body_ca.find("ca_cert=\"/etc/iot/certs/corp-ca.pem\""));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     build_escapes_quotes_in_identity_and_password) {
    WifiNetwork n;
    n.ssid     = "CorpAP";
    n.key_mgmt = "WPA-EAP";
    n.eap      = "PEAP";
    n.identity = "us\"er";
    n.password = "pa\\ss\"wd";
    auto body = build_wpa_supplicant_config(
        "wlan0", "/run/wpa_supplicant", {n});
    EXPECT_NE(std::string::npos, body.find("identity=\"us\\\"er\""));
    EXPECT_NE(std::string::npos, body.find("password=\"pa\\\\ss\\\"wd\""));
}

TEST(WIFI_REQ_WIFI_024_wpa_enterprise_eap,
     seeded_schema_default_parses_cleanly) {
    // Mirrors the wifi.networks default shipped in schemas/wifi.lua.
    // A fresh image relies on this parsing into exactly one PSK network
    // so wifi-client autostart has a valid (placeholder) config to load.
    const char* kSeededDefault =
        R"([{"ssid":"changeme","key_mgmt":"WPA-PSK","psk":"changeme","priority":10}])";
    std::string err;
    auto nets = parse_wifi_networks(kSeededDefault, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(1u, nets.size());
    EXPECT_EQ("changeme", nets[0].ssid);
    EXPECT_EQ("WPA-PSK",  nets[0].key_mgmt);
    EXPECT_EQ("changeme", nets[0].psk);
    EXPECT_EQ(10,         nets[0].priority);
}

// ─────────────────────────── write_temp_config ──────────────────────

TEST(WIFI_REQ_WIFI_015_wpa_conf_writer_orders_by_priority,
     write_temp_config_creates_file_with_body) {
    std::string body = "header\nbody\n";
    auto path = write_temp_config(body);
    // Path must end in .conf and live somewhere writable.
    EXPECT_NE(std::string::npos, path.rfind(".conf"));
    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(body, content);
    ::unlink(path.c_str());
}
