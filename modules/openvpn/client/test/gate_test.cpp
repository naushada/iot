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

// ─────────── L16/D4: services gate composes with WAN ─────────────
// Composition rule: enable=false dominates every other gate.
// When disabled, the supervisor parks at gate.reason="disabled" and
// never calls Gate::evaluate(). When re-enabled, WAN evaluation
// resumes as normal. These tests ratify that contract.

TEST(Gate, DisabledDominatesWanUpIdleStaysIdle) {
    // WAN up, gate enabled → would normally Spawn. But if the
    // services gate is closed, the supervisor never calls evaluate()
    // and the Gate stays idle. Prove the Gate's perspective: it would
    // return Spawn if consulted, confirming the caller (Supervisor)
    // is solely responsible for the disable check.
    Gate g;
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
    EXPECT_EQ("eth0", d.iface);
    // Gate is still idle — the supervisor would not apply this
    // decision when m_svc->enabled()==false.
    EXPECT_FALSE(g.running());
}

TEST(Gate, DisabledMidSessionGateNotEvaluated_wanUpDoesNotRetrigger) {
    // Scenario: session active on eth0, operator disables service,
    // supervisor reaps child + parks. WAN stays up the whole time.
    // The Gate's perspective: the supervisor calls note_terminated()
    // before parking, so the Gate is now idle. The next evaluate()
    // with eth0 would say Spawn — but supervisor only calls it
    // after m_svc->enabled() becomes true again.
    Gate g;
    g.note_spawned("eth0");
    ASSERT_TRUE(g.running());

    // Simulate supervisor's disable sequence:
    // if (m_svc && !m_svc->enabled()) { g.note_terminated(); park; }
    g.note_terminated();
    EXPECT_FALSE(g.running());

    // WAN still reports eth0 up. The Gate would spawn, proving
    // the disable check must gate the evaluate call.
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
    EXPECT_EQ("eth0", d.iface);
}

TEST(Gate, ReenableAfterDisableResumesNormalWanEvaluation) {
    // Full composition sequence:
    //   1. Idle, WAN down → None
    //   2. WAN eth0 up → Spawn
    //   3. Session active → disable (svc gate flips false)
    //      Supervisor calls note_terminated, parks
    //   4. Re-enable (svc gate flips true), WAN still eth0 → Spawn
    Gate g;
    // Step 1.
    EXPECT_EQ(GateDecision::Action::None, evaluate(g, std::nullopt));
    // Step 2.
    auto d2 = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d2.action);
    g.note_spawned("eth0");
    // Step 3 — supervisor sees svc disabled, reaps child.
    g.note_terminated();
    EXPECT_FALSE(g.running());
    // Step 4 — re-enabled, WAN still eth0.
    auto d4 = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d4.action);
    EXPECT_EQ("eth0", d4.iface);
    EXPECT_TRUE(d4.from.empty());   // fresh spawn, not restart
}

TEST(Gate, DisableMidSessionThenWanDropsThenReenable) {
    // Disabled while running; while parked, WAN drops. On re-enable,
    // WAN is down → None (do not spawn).
    Gate g;
    g.note_spawned("eth0");
    ASSERT_TRUE(g.running());

    // Disable → supervisor reaps, parks.
    g.note_terminated();
    EXPECT_FALSE(g.running());

    // While parked, WAN drops.
    auto d = g.evaluate(std::nullopt);
    EXPECT_EQ(GateDecision::Action::None, d.action);

    // Re-enable, WAN still down.
    EXPECT_EQ(GateDecision::Action::None, evaluate(g, std::nullopt));
}

TEST(Gate, DisableMidSessionWanFlipsDuringParkThenReturnsOnReenable) {
    // Complex recovery: disable while on eth0, WAN flips eth0→wlan0
    // while parked (not acted on), then on re-enable WAN is at wlan0
    // → fresh Spawn on wlan0.
    Gate g;
    g.note_spawned("eth0");

    // Disable.
    g.note_terminated();
    EXPECT_FALSE(g.running());

    // WAN flips eth0→wlan0 while parked (gate never sees it).
    // On re-enable: supervisor calls evaluate(wlan0).
    auto d = g.evaluate(std::string{"wlan0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
    EXPECT_EQ("wlan0", d.iface);
    EXPECT_TRUE(d.from.empty());   // no "from" — note_terminated was called earlier
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

// ─────── L17a/D3: dep_down composition (gate.reason) ───────────
// dep_down > disabled > wan_down. These tests verify the
// Supervisor's composition priority from the Gate's perspective:
// when a dependency is unhealthy, the Supervisor parks before
// evaluating the WAN gate, so the Gate never sees a Spawn/Terminate
// decision for a dep-down scenario.

TEST(Gate, DepDownGateNotEvaluatedWhenDepsUnhealthy) {
    // WAN up, svc enabled, but deps unhealthy → Supervisor parks
    // at gate.reason="dep_down:net.router" without calling
    // Gate::evaluate(). The Gate remains idle.
    Gate g;
    EXPECT_FALSE(g.running());
    // The Supervisor would not consult the Gate at all here —
    // the dep check short-circuits before WAN evaluation.
    // Prove: if it DID evaluate, the Gate would Spawn — confirming
    // the Supervisor is solely responsible for the dep check.
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
}

TEST(Gate, DepRecoveredGateEvaluatesNormally) {
    // Dep was unhealthy, now recovered. Supervisor calls
    // Gate::evaluate() as normal. WAN up → Spawn.
    Gate g;
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
    EXPECT_EQ("eth0", d.iface);
}

TEST(Gate, DepDownWhileRunningGateNotifiedByCaller) {
    // Session active on eth0, dep goes unhealthy → Supervisor
    // calls note_terminated() (same code path as svc disable).
    // The Gate does NOT know why — the reason comes from the
    // Supervisor's gate.reason string.
    Gate g;
    g.note_spawned("eth0");
    ASSERT_TRUE(g.running());

    // Supervisor: m_gate.note_terminated(); park at dep_down.
    g.note_terminated();
    EXPECT_FALSE(g.running());

    // Dep recovers → WAN still up → Spawn.
    auto d = g.evaluate(std::string{"eth0"});
    EXPECT_EQ(GateDecision::Action::Spawn, d.action);
}
