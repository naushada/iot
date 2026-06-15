/// D2 schema-file content tests for schemas/wifi.lua.
///
/// REQ-WIFI-004 — ds-server rejects bad-type sets. The hard
/// enforcement happens via ds-server's SchemaRegistry (already
/// covered by modules/data-store/test/schema_test.cpp). This file
/// keeps the unit-test scope to "the wifi.lua we ship declares the
/// surface our daemon uses, with the expected types + defaults."
/// The wire-level "bad set is SchemaRejected" assertion runs in
/// log/L15/smoke.sh against a real ds-server (D8).
///
/// REQ-WIFI-005 — every read key from main_impl.cpp's kReadKeys
/// MUST appear in wifi.lua with the documented default.
/// REQ-WIFI-006 — every write key from main_impl.cpp's kWriteKeys
/// MUST appear in wifi.lua with the right type; integer keys MUST
/// be typed integer; assoc.state is the documented string enum.

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "client.hpp"

namespace wifi_client {
const std::string_view* read_keys_data();
std::size_t             read_keys_size();
const std::string_view* write_keys_data();
std::size_t             write_keys_size();
} // namespace wifi_client

namespace {

/// Load the lua schema file. The relative path resolves from the
/// CTest working directory (the build dir), which is one level
/// below modules/wan/wifi/client/. CMake adds ".." as the test's
/// reference frame.
std::string load_schema() {
    // Try the canonical install location first (in-tree dev test
    // run via CTest from the build/ dir).
    const char* candidates[] = {
        "../schemas/wifi.lua",
        "schemas/wifi.lua",
        "modules/wan/wifi/client/schemas/wifi.lua",
        "/src/modules/wan/wifi/client/schemas/wifi.lua",
    };
    for (auto p : candidates) {
        std::ifstream in(p);
        if (in.good()) {
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }
    }
    return {};
}

/// Pull the `{ ... }` body of a key declaration. Returns empty if
/// the key isn't declared at all. The match is intentionally
/// permissive — Lua allows surprising whitespace + trailing
/// commas; we only assert what we care about (type, default, range).
std::string entry_body(const std::string& schema, const std::string& key) {
    // ["wifi.foo.bar"] = { ... }  — capture the braced body.
    std::string pattern = R"(\[\")" + key + R"(\"\]\s*=\s*\{([^}]*)\})";
    std::regex re(pattern);
    std::smatch m;
    if (std::regex_search(schema, m, re)) return m[1].str();
    return {};
}

bool has_type(const std::string& body, const std::string& type) {
    std::regex re("type\\s*=\\s*\"" + type + "\"");
    return std::regex_search(body, re);
}

bool has_default(const std::string& body, const std::string& expected) {
    // Default values for strings appear quoted; integers bare.
    std::regex quoted(R"(default\s*=\s*\")" + expected + R"(\")");
    std::regex bare("default\\s*=\\s*" + expected + R"(\b)");
    return std::regex_search(body, quoted) || std::regex_search(body, bare);
}

} // namespace

// ─────────────────────────── REQ-WIFI-005 ───────────────────────────

class WIFI_REQ_WIFI_005_read_keys_have_defaults : public ::testing::Test {
protected:
    std::string schema;
    void SetUp() override {
        schema = load_schema();
        ASSERT_FALSE(schema.empty())
            << "could not load schemas/wifi.lua from any candidate path; "
            << "test working dir was probably wrong";
    }
};

TEST_F(WIFI_REQ_WIFI_005_read_keys_have_defaults, every_read_key_is_declared) {
    for (std::size_t i = 0; i < wifi_client::read_keys_size(); ++i) {
        std::string k(wifi_client::read_keys_data()[i]);
        EXPECT_FALSE(entry_body(schema, k).empty())
            << "wifi.lua MUST declare " << k
            << " — main_impl.cpp kReadKeys lists it";
    }
}

TEST_F(WIFI_REQ_WIFI_005_read_keys_have_defaults, documented_defaults_match) {
    // The L15 plan §D2 + design.md fix the defaults; these MUST
    // not drift between the doc and the lua file.
    struct Pair { std::string key; std::string deflt; };
    const Pair pairs[] = {
        {"wifi.iface",             "wlan0"},
        {"wifi.ctrl.dir",          "/run/wpa_supplicant"},
        {"wifi.wpa.path",          "/usr/sbin/wpa_supplicant"},
        {"wifi.scan.interval.sec", "60"},
        {"wifi.scan.max.results",  "20"},
        {"wifi.scan.request",      "0"},
        {"wifi.dhcp.client",       "auto"},
    };
    for (const auto& p : pairs) {
        auto body = entry_body(schema, p.key);
        ASSERT_FALSE(body.empty()) << "missing key: " << p.key;
        EXPECT_TRUE(has_default(body, p.deflt))
            << p.key << " default expected " << p.deflt << ", body was:\n  " << body;
    }

    // wifi.networks is special-cased: its default is a JSON array whose
    // embedded { } and quotes defeat the brace-naive entry_body() helper.
    // Assert against the full schema text instead. The default must seed
    // one placeholder PSK network (autostart-on-boot, REQ-WIFI-024), NOT
    // the old empty "[]".
    EXPECT_EQ(std::string::npos, schema.find(R"(default = "[]")"))
        << "wifi.networks must no longer default to empty []";
    EXPECT_NE(std::string::npos,
              schema.find(R"("ssid":"changeme")"))
        << "wifi.networks default must seed a placeholder network";
    EXPECT_NE(std::string::npos,
              schema.find(R"("key_mgmt":"WPA-PSK")"))
        << "seeded default network must be a WPA-PSK entry";
}

// ─────────────────────────── REQ-WIFI-006 ───────────────────────────

class WIFI_REQ_WIFI_006_write_keys_typed : public ::testing::Test {
protected:
    std::string schema;
    void SetUp() override {
        schema = load_schema();
        ASSERT_FALSE(schema.empty());
    }
};

TEST_F(WIFI_REQ_WIFI_006_write_keys_typed, every_write_key_is_declared) {
    for (std::size_t i = 0; i < wifi_client::write_keys_size(); ++i) {
        std::string k(wifi_client::write_keys_data()[i]);
        EXPECT_FALSE(entry_body(schema, k).empty())
            << "wifi.lua MUST declare " << k
            << " — main_impl.cpp kWriteKeys lists it";
    }
}

TEST_F(WIFI_REQ_WIFI_006_write_keys_typed, integer_keys_typed_integer) {
    const std::string int_keys[] = {
        "wifi.signal.rssi",
        "wifi.scan.last.unix",
        "wifi.pid.wpa",
        "wifi.pid.dhcp",
        "wifi.dhcp.lease.sec",
        "wifi.dhcp.obtained.unix",
    };
    for (const auto& k : int_keys) {
        auto body = entry_body(schema, k);
        ASSERT_FALSE(body.empty()) << "missing key: " << k;
        EXPECT_TRUE(has_type(body, "integer"))
            << k << " expected type=integer, body was:\n  " << body;
    }
}

TEST_F(WIFI_REQ_WIFI_006_write_keys_typed, string_keys_typed_string) {
    const std::string str_keys[] = {
        "wifi.assoc.state",
        "wifi.assoc.ssid",
        "wifi.assoc.bssid",
        "wifi.scan.results",
        "wifi.dhcp.state",
        "wifi.dhcp.ip",
        "wifi.dhcp.mask",
        "wifi.dhcp.gateway",
        "wifi.dhcp.dns",
        "wifi.dhcp.domain",
        "wifi.last.error",
    };
    for (const auto& k : str_keys) {
        auto body = entry_body(schema, k);
        ASSERT_FALSE(body.empty()) << "missing key: " << k;
        EXPECT_TRUE(has_type(body, "string"))
            << k << " expected type=string, body was:\n  " << body;
    }
}

// ─────────────────────────── REQ-WIFI-004 ───────────────────────────
// File-level sanity that the schema is loadable: namespace declared,
// table closes cleanly. The actual "bad set rejected" wire test
// lives in log/L15/smoke.sh against a real ds-server.

TEST(WIFI_REQ_WIFI_004_schema_loadable, declares_wifi_namespace) {
    auto schema = load_schema();
    ASSERT_FALSE(schema.empty());
    EXPECT_NE(std::string::npos, schema.find("namespace = \"wifi\""))
        << "wifi.lua must declare its namespace as 'wifi' so ds-server's "
        << "WARN-on-duplicate path treats this file as the wifi owner";
}

TEST(WIFI_REQ_WIFI_004_schema_loadable, return_statement_at_top_level) {
    auto schema = load_schema();
    ASSERT_FALSE(schema.empty());
    // ds-server's SchemaRegistry runs the file via dofile() and
    // expects the top-level chunk to RETURN the table.
    EXPECT_NE(std::string::npos, schema.find("return {"))
        << "wifi.lua must return a table at top level; without it "
        << "SchemaRegistry::load_one() raises";
}

TEST(WIFI_REQ_WIFI_004_schema_loadable, scan_max_results_has_documented_range) {
    auto schema = load_schema();
    ASSERT_FALSE(schema.empty());
    auto body = entry_body(schema, "wifi.scan.max.results");
    ASSERT_FALSE(body.empty());
    EXPECT_TRUE(has_type(body, "integer"));
    EXPECT_NE(std::string::npos, body.find("min = 1"))
        << "wifi.scan.max.results min=1 documented in design.md, body:\n  " << body;
    EXPECT_NE(std::string::npos, body.find("max = 200"))
        << "wifi.scan.max.results max=200 documented in design.md, body:\n  " << body;
}
