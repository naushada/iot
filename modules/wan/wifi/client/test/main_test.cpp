/// Unit tests for wifi-client D1 surface: CLI parse + --dump keylist.
///
/// REQ-WIFI-002 — parse_cli recognises every documented flag and
/// rejects unknown args with exit_code=2.
/// REQ-WIFI-003 — v0_dump_wifi_keys streams every wifi.* key
/// (read + write) into the provided ostream, returns ok, and does
/// not perform any I/O against ds-server (the socket argument is
/// accepted but unused in this op).

#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "client.hpp"

// Test-only accessors defined in main_impl.cpp.
namespace wifi_client {
const std::string_view* read_keys_data();
std::size_t             read_keys_size();
const std::string_view* write_keys_data();
std::size_t             write_keys_size();
} // namespace wifi_client

namespace {

/// Build a fake argv from a vector of strings so each test can
/// craft its own command line without strdup-and-leak hackery.
/// The returned vector OWNS the char* storage via the underlying
/// std::string lifetimes — pass it by reference, don't move it.
class FakeArgv {
public:
    FakeArgv(std::initializer_list<std::string> args) : m_storage(args) {
        for (auto& s : m_storage) m_argv.push_back(s.data());
        m_argv.push_back(nullptr);
    }
    int    argc() const { return static_cast<int>(m_storage.size()); }
    char** argv()       { return m_argv.data(); }
private:
    std::vector<std::string> m_storage;
    std::vector<char*>       m_argv;
};

} // namespace

// ─────────────────────────── REQ-WIFI-002 ───────────────────────────

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, defaults_when_no_args) {
    FakeArgv a{"wifi-client"};
    auto pc = wifi_client::parse_cli(a.argc(), a.argv());
    EXPECT_EQ(0, pc.exit_code);
    EXPECT_TRUE(pc.err.empty());
    EXPECT_EQ("",                       pc.sock);
    EXPECT_EQ("/usr/sbin/wpa_supplicant", pc.wpa_path);
    EXPECT_EQ("wlan0",                   pc.iface);
    EXPECT_EQ("/run/wpa_supplicant",     pc.ctrl_dir);
    EXPECT_FALSE(pc.dump);
    EXPECT_FALSE(pc.once);
    EXPECT_FALSE(pc.help);
}

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, every_known_flag) {
    FakeArgv a{"wifi-client",
               "--ds-sock=/tmp/ds.sock",
               "--wpa=/usr/local/sbin/wpa_supplicant",
               "--iface=wlan1",
               "--ctrl-dir=/var/run/wpa",
               "--dump",
               "--once"};
    auto pc = wifi_client::parse_cli(a.argc(), a.argv());
    ASSERT_EQ(0, pc.exit_code) << pc.err;
    EXPECT_EQ("/tmp/ds.sock",                  pc.sock);
    EXPECT_EQ("/usr/local/sbin/wpa_supplicant", pc.wpa_path);
    EXPECT_EQ("wlan1",                          pc.iface);
    EXPECT_EQ("/var/run/wpa",                   pc.ctrl_dir);
    EXPECT_TRUE(pc.dump);
    EXPECT_TRUE(pc.once);
}

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, help_flag_short_and_long) {
    {
        FakeArgv a{"wifi-client", "--help"};
        auto pc = wifi_client::parse_cli(a.argc(), a.argv());
        EXPECT_EQ(0, pc.exit_code);
        EXPECT_TRUE(pc.help);
    }
    {
        FakeArgv a{"wifi-client", "-h"};
        auto pc = wifi_client::parse_cli(a.argc(), a.argv());
        EXPECT_EQ(0, pc.exit_code);
        EXPECT_TRUE(pc.help);
    }
}

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, unknown_flag_rejected_with_exit_2) {
    FakeArgv a{"wifi-client", "--bogus"};
    auto pc = wifi_client::parse_cli(a.argc(), a.argv());
    EXPECT_EQ(2, pc.exit_code);
    EXPECT_NE(std::string::npos, pc.err.find("--bogus"))
        << "expected the error to name the offending arg, got: " << pc.err;
}

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, bare_positional_rejected) {
    // Bare positional args have never been a thing in iot daemon CLIs.
    // Treating them as unknown matches openvpn-client's behaviour.
    FakeArgv a{"wifi-client", "wlan0"};
    auto pc = wifi_client::parse_cli(a.argc(), a.argv());
    EXPECT_EQ(2, pc.exit_code);
}

TEST(WIFI_REQ_WIFI_002_cli_parses_known_and_rejects_unknown, malformed_kv_unknown_rejected) {
    // --iface (no '=') isn't recognised as the iface flag; it's a
    // bare flag, so parse_cli should reject it.
    FakeArgv a{"wifi-client", "--iface"};
    auto pc = wifi_client::parse_cli(a.argc(), a.argv());
    EXPECT_EQ(2, pc.exit_code);
}

// ─────────────────────────── REQ-WIFI-003 ───────────────────────────

TEST(WIFI_REQ_WIFI_003_dump_lists_keys_without_ds, dump_returns_ok_with_empty_socket) {
    std::ostringstream sink;
    // Empty socket path; the function MUST NOT attempt to connect.
    auto rs = wifi_client::v0_dump_wifi_keys("", sink);
    EXPECT_TRUE(rs.ok) << rs.err;
    EXPECT_EQ(0, rs.code);
}

TEST(WIFI_REQ_WIFI_003_dump_lists_keys_without_ds, dump_lists_every_read_key) {
    std::ostringstream sink;
    wifi_client::v0_dump_wifi_keys("ignored", sink);
    auto out = sink.str();
    for (std::size_t i = 0; i < wifi_client::read_keys_size(); ++i) {
        auto k = std::string(wifi_client::read_keys_data()[i]);
        EXPECT_NE(std::string::npos, out.find(k))
            << "expected " << k << " in --dump output, got:\n" << out;
    }
}

TEST(WIFI_REQ_WIFI_003_dump_lists_keys_without_ds, dump_lists_every_write_key) {
    std::ostringstream sink;
    wifi_client::v0_dump_wifi_keys("ignored", sink);
    auto out = sink.str();
    for (std::size_t i = 0; i < wifi_client::write_keys_size(); ++i) {
        auto k = std::string(wifi_client::write_keys_data()[i]);
        EXPECT_NE(std::string::npos, out.find(k))
            << "expected " << k << " in --dump output, got:\n" << out;
    }
}

TEST(WIFI_REQ_WIFI_003_dump_lists_keys_without_ds, dump_separates_read_and_write_sections) {
    std::ostringstream sink;
    wifi_client::v0_dump_wifi_keys("", sink);
    auto out = sink.str();
    auto reads  = out.find("Read keys");
    auto writes = out.find("Write keys");
    ASSERT_NE(std::string::npos, reads);
    ASSERT_NE(std::string::npos, writes);
    EXPECT_LT(reads, writes) << "Read section should precede Write section";
}

// ─────────────────────────── REQ-WIFI-026 ───────────────────────────
// (Sanity check that the D1 source files don't emit any std::cout
// from places other than the operator-facing CLI surface. Not an
// invariant we can fully assert here — REQ-WIFI-026 is enforced by
// the manual grep in the traceability matrix. This test just guards
// the easy regression: v0_dump_wifi_keys must write to its ostream,
// not to global cout.)

TEST(WIFI_REQ_WIFI_026_diag_logs_via_ACE, dump_does_not_write_to_global_cout) {
    // Capture global cout; if any of the D1 functions accidentally
    // write to it, the captured string will be non-empty.
    auto* prev = std::cout.rdbuf();
    std::ostringstream cout_sink;
    std::cout.rdbuf(cout_sink.rdbuf());
    std::ostringstream explicit_sink;
    wifi_client::v0_dump_wifi_keys("", explicit_sink);
    std::cout.rdbuf(prev);
    EXPECT_TRUE(cout_sink.str().empty())
        << "v0_dump_wifi_keys wrote to global cout instead of its ostream arg: "
        << cout_sink.str();
    EXPECT_FALSE(explicit_sink.str().empty());
}
