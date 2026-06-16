/// Unit tests for ctrl::Parser. The wire-level Client (connect +
/// send + recv via ACE_LSOCK_Dgram) is exercised by log/L15/smoke.sh
/// against fake-wpa.sh — the protocol's stable, the harder thing
/// to fake is realistic event traces, which the smoke covers.
///
/// REQ-WIFI-010 — connect sends ATTACH: covered at the wire level in
/// D8 smoke; the Client's connect() implementation is documented +
/// inspection-tested. No unit test for it here — driving a real
/// socket from a single-threaded gtest would just duplicate the
/// smoke.
/// REQ-WIFI-011 — request classifies ok vs FAIL: tested via the
/// reply-shape parser indirectly (FAIL detection is one line in
/// ctrl.cpp). Wire-level integration in D8.
/// REQ-WIFI-012 — event parser classifies every documented kind.
/// REQ-WIFI-013 — event parser extracts ssid / bssid / reason.
/// NFR-WIFI-005 — parser is O(line-length); demonstrated by feeding
/// a large but bounded line and asserting non-pathological behavior.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctrl.hpp"

using wifi_client::ctrl::Client;
using wifi_client::ctrl::CtrlEvent;
using wifi_client::ctrl::Parser;

namespace {

/// Load the canned event trace and return one event per non-empty
/// line. The fixture sits alongside the test source under
/// test/data/wpa_events.txt; the test runner cwd's to the build
/// dir, so paths are tried from there.
std::vector<std::string> load_fixture_lines() {
    const char* candidates[] = {
        "../test/data/wpa_events.txt",
        "test/data/wpa_events.txt",
        "modules/wan/wifi/client/test/data/wpa_events.txt",
        "/src/modules/wan/wifi/client/test/data/wpa_events.txt",
    };
    for (auto p : candidates) {
        std::ifstream in(p);
        if (!in.good()) continue;
        std::vector<std::string> out;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) out.push_back(line);
        }
        return out;
    }
    return {};
}

} // namespace

// ─────────────────────────── REQ-WIFI-012 ───────────────────────────

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, scan_started) {
    auto ev = Parser::classify("<3>CTRL-EVENT-SCAN-STARTED");
    EXPECT_EQ(CtrlEvent::Kind::ScanStarted, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, scan_results) {
    auto ev = Parser::classify("<3>CTRL-EVENT-SCAN-RESULTS");
    EXPECT_EQ(CtrlEvent::Kind::ScanResults, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, connected) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff completed [id=0 id_str=]");
    EXPECT_EQ(CtrlEvent::Kind::Connected, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, disconnected) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:dd:ee:ff reason=3");
    EXPECT_EQ(CtrlEvent::Kind::Disconnected, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, assoc_reject) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:dd:ee:ff status_code=17");
    EXPECT_EQ(CtrlEvent::Kind::AssocReject, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, auth_reject) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-AUTH-REJECT bssid=aa:bb:cc:dd:ee:ff status_code=15");
    EXPECT_EQ(CtrlEvent::Kind::AuthReject, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds, terminating) {
    auto ev = Parser::classify("<3>CTRL-EVENT-TERMINATING");
    EXPECT_EQ(CtrlEvent::Kind::Terminating, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds,
     unrecognised_falls_to_unknown_no_throw) {
    auto ev = Parser::classify("<3>CTRL-EVENT-FUTURE-WPA-MAY-INTRODUCE foo=bar");
    EXPECT_EQ(CtrlEvent::Kind::Unknown, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds,
     missing_priority_prefix_still_classifies) {
    // Some wpa_supplicant builds (or our fake-wpa.sh harness) may
    // omit the <N> indicator. The parser must still classify.
    auto ev = Parser::classify("CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff completed");
    EXPECT_EQ(CtrlEvent::Kind::Connected, ev.kind);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds,
     trailing_cr_lf_stripped) {
    auto ev = Parser::classify("<3>CTRL-EVENT-SCAN-RESULTS\r\n");
    EXPECT_EQ(CtrlEvent::Kind::ScanResults, ev.kind);
    EXPECT_EQ("CTRL-EVENT-SCAN-RESULTS", ev.raw);
}

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds,
     empty_input_maps_to_unknown) {
    auto ev = Parser::classify("");
    EXPECT_EQ(CtrlEvent::Kind::Unknown, ev.kind);
    EXPECT_TRUE(ev.raw.empty());
}

// ─────────────────────────── REQ-WIFI-013 ───────────────────────────

TEST(WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason,
     connected_bssid_extracted) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff completed [id=0 id_str=HomeAP]");
    ASSERT_EQ(CtrlEvent::Kind::Connected, ev.kind);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", ev.bssid);
    EXPECT_EQ("HomeAP", ev.ssid);
}

TEST(WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason,
     disconnected_bssid_and_reason_extracted) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:dd:ee:ff reason=3 locally_generated=1");
    ASSERT_EQ(CtrlEvent::Kind::Disconnected, ev.kind);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", ev.bssid);
    EXPECT_EQ("3", ev.reason);
}

TEST(WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason,
     assoc_reject_status_code_extracted) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-ASSOC-REJECT bssid=11:22:33:44:55:66 status_code=17");
    ASSERT_EQ(CtrlEvent::Kind::AssocReject, ev.kind);
    EXPECT_EQ("11:22:33:44:55:66", ev.bssid);
    EXPECT_EQ("17", ev.reason);
}

TEST(WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason,
     auth_reject_status_code_extracted) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-AUTH-REJECT bssid=11:22:33:44:55:66 auth_type=0 auth_transaction=2 status_code=15");
    ASSERT_EQ(CtrlEvent::Kind::AuthReject, ev.kind);
    EXPECT_EQ("11:22:33:44:55:66", ev.bssid);
    EXPECT_EQ("15", ev.reason);
}

TEST(WIFI_REQ_WIFI_013_event_parser_extracts_ssid_bssid_reason,
     connected_without_id_str_ssid_empty) {
    auto ev = Parser::classify(
        "<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff completed [id=0 id_str=]");
    ASSERT_EQ(CtrlEvent::Kind::Connected, ev.kind);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", ev.bssid);
    EXPECT_TRUE(ev.ssid.empty());
}

// ─────────────────────────── Fixture-driven ───────────────────────

TEST(WIFI_REQ_WIFI_012_event_parser_classifies_all_kinds,
     fixture_file_classifies_every_documented_kind) {
    auto lines = load_fixture_lines();
    ASSERT_FALSE(lines.empty())
        << "could not load test/data/wpa_events.txt from any candidate path";

    bool saw[8] = {false, false, false, false, false, false, false, false};
    for (const auto& line : lines) {
        auto ev = Parser::classify(line);
        switch (ev.kind) {
        case CtrlEvent::Kind::ScanStarted:   saw[0] = true; break;
        case CtrlEvent::Kind::ScanResults:   saw[1] = true; break;
        case CtrlEvent::Kind::Connected:     saw[2] = true; break;
        case CtrlEvent::Kind::Disconnected:  saw[3] = true; break;
        case CtrlEvent::Kind::AssocReject:   saw[4] = true; break;
        case CtrlEvent::Kind::AuthReject:    saw[5] = true; break;
        case CtrlEvent::Kind::Terminating:   saw[6] = true; break;
        case CtrlEvent::Kind::Unknown:       saw[7] = true; break;
        }
    }
    EXPECT_TRUE(saw[0]) << "ScanStarted not seen in fixture";
    EXPECT_TRUE(saw[1]) << "ScanResults not seen in fixture";
    EXPECT_TRUE(saw[2]) << "Connected not seen in fixture";
    EXPECT_TRUE(saw[3]) << "Disconnected not seen in fixture";
    EXPECT_TRUE(saw[4]) << "AssocReject not seen in fixture";
    EXPECT_TRUE(saw[5]) << "AuthReject not seen in fixture";
    EXPECT_TRUE(saw[6]) << "Terminating not seen in fixture";
    EXPECT_TRUE(saw[7]) << "Unknown not seen in fixture (forward-compat test)";
}

// ─────────────────────────── NFR-WIFI-005 ───────────────────────────

TEST(WIFI_NFR_WIFI_005_parser_bounded_per_line,
     long_unknown_line_does_not_explode) {
    // Bound: a malformed event line of ~16KB classifies without
    // pathological time or alloc. We assert no throw + Unknown kind.
    std::string huge = "<3>";
    huge.append(16 * 1024, 'X');
    auto ev = Parser::classify(huge);
    EXPECT_EQ(CtrlEvent::Kind::Unknown, ev.kind);
    EXPECT_GT(ev.raw.size(), 1000u);  // priority stripped, body kept
}

// ─────────────────────────── REQ-WIFI-010 ───────────────────────────
// Wire-level: connect() must put "ATTACH" on the socket with NO trailing
// terminator. Real wpa_supplicant matches control verbs with an exact
// os_strcmp(), so a trailing '\n' comes back "UNKNOWN COMMAND" and the
// daemon never attaches (the bug that wedged WiFi on hardware; the
// line-based smoke fake rstrip'd the newline so it never surfaced there).

TEST(WIFI_REQ_WIFI_010_ctrl_attach_wire,
     attach_sent_without_trailing_newline) {
    char dir[] = "/tmp/wpa_ctrl_wire_XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(dir));
    const std::string srv_path = std::string(dir) + "/wlan0";

    int srv = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(srv, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, srv_path.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(0, ::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));

    // Fake wpa: recv the ATTACH datagram verbatim, reply "OK".
    std::string got;
    std::thread wpa([&] {
        char buf[256];
        sockaddr_un from{};
        socklen_t   fl = sizeof(from);
        ssize_t n = ::recvfrom(srv, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fl);
        if (n > 0) {
            got.assign(buf, static_cast<std::size_t>(n));
            ::sendto(srv, "OK\n", 3, 0,
                     reinterpret_cast<sockaddr*>(&from), fl);
        }
    });

    Client c;
    const bool attached = c.connect(srv_path);
    wpa.join();
    ::close(srv);
    ::unlink(srv_path.c_str());
    ::rmdir(dir);

    EXPECT_TRUE(attached) << "connect() must accept an OK reply to ATTACH";
    EXPECT_EQ("ATTACH", got)
        << "ATTACH must hit the wire with no trailing newline";
}
