#include "gate.hpp"

namespace openvpn_client {

GateDecision Gate::evaluate(const std::optional<std::string>& target) const {
    const bool target_up = target.has_value() && !target->empty();

    if (!target_up) {
        if (running()) {
            GateDecision d;
            d.action = GateDecision::Action::Terminate;
            d.from   = m_bound;
            return d;
        }
        return {};
    }

    // Target is up.
    if (!running()) {
        GateDecision d;
        d.action = GateDecision::Action::Spawn;
        d.iface  = *target;
        return d;
    }
    if (m_bound == *target) {
        return {};
    }
    GateDecision d;
    d.action = GateDecision::Action::Restart;
    d.iface  = *target;
    d.from   = m_bound;
    return d;
}

} // namespace openvpn_client
