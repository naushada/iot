#ifndef __openvpn_client_gate_hpp__
#define __openvpn_client_gate_hpp__

/// Pure WAN-gate policy for openvpn-client.
///
/// net-router publishes `net.iface.active` — the highest-priority
/// OPER-UP WAN iface (eth0 / wlan0 / wwan0 / …). The supervisor
/// asks the Gate, on every snapshot, "given my current bound state,
/// what should I do?" and the Gate returns a Decision.
///
/// Pure: no I/O, no threading. Tests drive it through scenarios
/// without touching processes or the data-store. The supervisor
/// notifies the Gate of state transitions via note_spawned /
/// note_terminated; the Gate never spawns or terminates itself.

#include <optional>
#include <string>

namespace openvpn_client {

struct GateDecision {
    enum class Action {
        None,      ///< Bound matches target — leave the child alone.
        Spawn,     ///< Idle → start a session bound to iface.
        Terminate, ///< Running → tear down (WAN went down).
        Restart,   ///< Running → tear down + respawn (iface changed).
    };

    Action      action = Action::None;
    std::string iface;  ///< Target iface for Spawn / Restart.
    std::string from;   ///< Previously bound iface for Terminate / Restart.
};

class Gate {
public:
    /// True when a session is currently bound to a WAN iface.
    bool running() const { return !m_bound.empty(); }

    /// Iface the current session (if any) is bound to; "" otherwise.
    const std::string& bound() const { return m_bound; }

    /// Decide what to do given an incoming WAN snapshot. `target`
    /// nullopt or empty-valued means "WAN is down". Pure — does NOT
    /// transition the Gate's own state; the caller must invoke
    /// note_spawned / note_terminated after acting on the decision.
    GateDecision evaluate(const std::optional<std::string>& target) const;

    void note_spawned(const std::string& iface)    { m_bound = iface; }
    void note_terminated()                          { m_bound.clear(); }

private:
    std::string m_bound;  ///< Currently-bound iface; empty when idle.
};

} // namespace openvpn_client

#endif /* __openvpn_client_gate_hpp__ */
