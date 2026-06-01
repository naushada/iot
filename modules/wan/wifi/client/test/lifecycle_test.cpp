/// Pure FSM tests for wifi-client's Lifecycle (L15/D6).
/// REQ-WIFI-018 — only documented transitions; same state suppressed
/// (NFR-WIFI-004 part 1).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ctrl.hpp"
#include "lifecycle.hpp"

using wifi_client::Lifecycle;
using wifi_client::ctrl::CtrlEvent;

namespace {

/// Capture-spy for the Sinks. The Lifecycle's contract is "Sinks
/// fire on specific events" — we verify by recording every call
/// and asserting the trace matches expectations.
struct Spy {
    std::vector<std::string> states;           ///< set_state calls
    std::vector<std::pair<std::string, std::string>> connects;
    std::vector<std::string> disconnects;
    int                      scan_results = 0;
    std::vector<std::string> rejects;

    Lifecycle::Sinks sinks() {
        return {
            [this](std::string_view s) { states.emplace_back(s); },
            [this](const std::string& ssid, const std::string& bssid) {
                connects.emplace_back(ssid, bssid);
            },
            [this](const std::string& r) { disconnects.push_back(r); },
            [this]() { ++scan_results; },
            [this](const std::string& e) { rejects.push_back(e); },
        };
    }
};

CtrlEvent ev_kind(CtrlEvent::Kind k) {
    CtrlEvent e;
    e.kind = k;
    return e;
}

CtrlEvent ev_connected(std::string ssid, std::string bssid) {
    CtrlEvent e;
    e.kind = CtrlEvent::Kind::Connected;
    e.ssid = std::move(ssid);
    e.bssid = std::move(bssid);
    return e;
}

CtrlEvent ev_disconnected(std::string reason) {
    CtrlEvent e;
    e.kind = CtrlEvent::Kind::Disconnected;
    e.reason = std::move(reason);
    return e;
}

CtrlEvent ev_reject(CtrlEvent::Kind k, std::string reason) {
    CtrlEvent e;
    e.kind = k;
    e.reason = std::move(reason);
    return e;
}

} // namespace

// ─────────────────────── REQ-WIFI-018 ───────────────────────

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     initial_state_disconnected) {
    Spy s;
    Lifecycle fsm(s.sinks());
    EXPECT_EQ("disconnected", std::string(fsm.current()));
    EXPECT_TRUE(s.states.empty()) << "no initial set_state expected";
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     disconnected_to_scanning_on_scan_started) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    EXPECT_EQ("scanning", std::string(fsm.current()));
    ASSERT_EQ(1u, s.states.size());
    EXPECT_EQ("scanning", s.states[0]);
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     scanning_to_connected_on_connected_event) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    fsm.step(ev_connected("HomeAP", "aa:bb:cc:dd:ee:ff"));
    EXPECT_EQ("connected", std::string(fsm.current()));
    ASSERT_EQ(2u, s.states.size());
    EXPECT_EQ("connected", s.states[1]);
    ASSERT_EQ(1u, s.connects.size());
    EXPECT_EQ("HomeAP", s.connects[0].first);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", s.connects[0].second);
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     connected_to_disconnected_on_disconnected_event) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    fsm.step(ev_connected("X", "11:22:33:44:55:66"));
    fsm.step(ev_disconnected("3"));
    EXPECT_EQ("disconnected", std::string(fsm.current()));
    ASSERT_EQ(1u, s.disconnects.size());
    EXPECT_EQ("3", s.disconnects[0]);
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     assoc_reject_pushes_scanning_and_fires_reject) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    fsm.step(ev_reject(CtrlEvent::Kind::AssocReject, "17"));
    EXPECT_EQ("scanning", std::string(fsm.current()));
    ASSERT_EQ(1u, s.rejects.size());
    EXPECT_EQ("assoc_reject:17", s.rejects[0]);
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     auth_reject_classifies_separately) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    fsm.step(ev_reject(CtrlEvent::Kind::AuthReject, "15"));
    ASSERT_EQ(1u, s.rejects.size());
    EXPECT_EQ("auth_reject:15", s.rejects[0]);
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     terminating_yields_exited) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::Terminating));
    EXPECT_EQ("exited", std::string(fsm.current()));
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     unknown_event_is_a_noop) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::Unknown));
    EXPECT_EQ("disconnected", std::string(fsm.current()));
    EXPECT_TRUE(s.states.empty());
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     scan_results_fires_callback_but_not_state_transition) {
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_kind(CtrlEvent::Kind::ScanStarted));
    s.states.clear();
    fsm.step(ev_kind(CtrlEvent::Kind::ScanResults));
    EXPECT_EQ(1, s.scan_results);
    EXPECT_TRUE(s.states.empty()) << "scan-results should not transition state";
}

TEST(WIFI_REQ_WIFI_018_fsm_documented_transitions_only,
     same_state_re_entry_suppressed) {
    // NFR-WIFI-004: write only on actual transitions. Two CONNECTED
    // events in a row should fire on_connected twice but set_state
    // exactly once.
    Spy s;
    Lifecycle fsm(s.sinks());
    fsm.step(ev_connected("A", "00"));
    fsm.step(ev_connected("A", "00"));
    EXPECT_EQ(1u, s.states.size())
        << "expected one transition, got: " << s.states.size();
    EXPECT_EQ(2u, s.connects.size());
}
