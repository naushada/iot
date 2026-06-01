/// Pure unit tests for the WAN-gate FSM. No process spawn, no
/// data-store — just feed snapshots and assert decisions. Supervisor's
/// I/O wiring (DsBridge subscription, mgmt event_loop interruption)
/// is exercised by the L14 smoke against a real ds-server.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "gate.hpp"

using openvpn_client::Gate;
using openvpn_client::GateDecision;

namespace {

GateDecision::Action evaluate(const Gate& g,
                              std::optional<std::string> target) {
    return g.evaluate(target).action;
}

} // namespace

TEST(Gate, IdleStaysIdleWhenWanDown) {
    Gate g;
    EXPECT_FALSE(g.running());
    EXPECT_EQ(GateDecision::Action::None, evaluate(g, std::nullopt));
    EXPECT_EQ(GateDecision::Action::None, evaluate(g, std::string{}));
}

TEST(Gate, IdleSpawnsWhenWanComesUp) {
    Gate g;
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
    EXPECT_EQ("eth0", d.iface);
    EXPECT_TRUE(d.from.empty());
}

TEST(Gate, RunningOnSameIfaceNoOp) {
    Gate g;
    g.note_spawned("eth0");
    ASSERT_TRUE(g.running());
    EXPECT_EQ(GateDecision::Action::None,
              evaluate(g, std::string{"eth0"}));
}

TEST(Gate, RunningTerminatesWhenWanDrops) {
    Gate g;
    g.note_spawned("eth0");
    auto d = g.evaluate(std::nullopt);
    EXPECT_EQ(GateDecision::Action::Terminate, d.action);
    EXPECT_EQ("eth0", d.from);
    EXPECT_TRUE(d.iface.empty());
}

TEST(Gate, RunningTerminatesWhenWanGoesEmptyString) {
    // net-router writes an empty string when no iface qualifies.
    // The Supervisor layer collapses "" → nullopt before evaluate(),
    // so the Gate itself shouldn't need to handle "" — but defend
    // anyway so a future caller refactor can't silently regress.
    Gate g;
    g.note_spawned("wlan0");
    auto d = g.evaluate(std::string{});
    EXPECT_EQ(GateDecision::Action::Terminate, d.action);
    EXPECT_EQ("wlan0", d.from);
}

TEST(Gate, RunningRestartsOnIfaceChange) {
    Gate g;
    g.note_spawned("eth0");
    auto d = g.evaluate(std::string{"wlan0"});
    EXPECT_EQ(GateDecision::Action::Restart, d.action);
    EXPECT_EQ("eth0",  d.from);
    EXPECT_EQ("wlan0", d.iface);
}

TEST(Gate, NoteTerminatedClearsBound) {
    Gate g;
    g.note_spawned("eth0");
    g.note_terminated();
    EXPECT_FALSE(g.running());
    EXPECT_TRUE(g.bound().empty());
    // Subsequent evaluate now decides as if idle.
    EXPECT_EQ(GateDecision::Action::Spawn,
              evaluate(g, std::string{"eth0"}));
}

TEST(Gate, FullPriorityRotationSequence) {
    // Cellular fallback story:
    //   1) boot with no WAN
    //   2) wlan0 comes up   → spawn
    //   3) eth0 plugs in    → restart (eth higher prio)
    //   4) cable unplugged  → restart back to wlan0
    //   5) wifi drops too   → terminate (idle, waiting on cellular)
    //   6) wwan0 dials      → spawn
    Gate g;
    EXPECT_EQ(GateDecision::Action::None,      evaluate(g, std::nullopt));

    auto d1 = g.evaluate(std::string{"wlan0"});
    EXPECT_EQ(GateDecision::Action::Spawn,     d1.action);
    g.note_spawned(d1.iface);

    auto d2 = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Restart,   d2.action);
    EXPECT_EQ("wlan0", d2.from);
    g.note_terminated();
    g.note_spawned(d2.iface);

    auto d3 = g.evaluate(std::string{"wlan0"});
    EXPECT_EQ(GateDecision::Action::Restart,   d3.action);
    EXPECT_EQ("eth0",  d3.from);
    g.note_terminated();
    g.note_spawned(d3.iface);

    auto d4 = g.evaluate(std::nullopt);
    EXPECT_EQ(GateDecision::Action::Terminate, d4.action);
    EXPECT_EQ("wlan0", d4.from);
    g.note_terminated();

    auto d5 = g.evaluate(std::string{"wwan0"});
    EXPECT_EQ(GateDecision::Action::Spawn,     d5.action);
    EXPECT_EQ("wwan0", d5.iface);
}
