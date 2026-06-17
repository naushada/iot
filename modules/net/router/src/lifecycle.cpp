#include "lifecycle.hpp"

namespace net_router {

const char* Lifecycle::state_name(State s) {
    switch (s) {
        case State::Init:       return "init";
        case State::NeedConfig: return "need-config";
        case State::Steady:     return "steady";
        case State::Failed:     return "failed";
    }
    return "?";
}

void Lifecycle::transition(State next) {
    if (next == m_state) return;
    m_state = next;
    if (m_sinks.set_state) m_sinks.set_state(state_name(next));
}

Lifecycle::State Lifecycle::step(const Inputs& in) {
    // net.lwm2m.target.ip is OPTIONAL. It only gates the DNAT/forward
    // rules — build_nft_ruleset() omits that chain when the target (or
    // the port list) is empty — NOT whether the router runs. The router's
    // real job (WAN election, base firewall, route metrics) proceeds with
    // or without a DNAT target, so an unprovisioned device still gets an
    // uplink and dependent services (lwm2m/openvpn) come up instead of
    // being wedged behind a config key. State now tracks the WAN uplink:
    // NeedConfig means "no usable WAN yet", Steady means "uplink up".

    // 1) Build the ruleset. Re-apply only when text actually changed —
    //    nft -f is idempotent but a no-op apply still incurs the
    //    parser + transaction cost.
    nft::State ns;
    ns.tun_dev          = in.tun_dev;
    ns.lwm2m_target_ip  = in.lwm2m_target_ip;
    ns.lwm2m_target_port = in.lwm2m_target_port;
    ns.forward_ports    = in.forward_ports;
    ns.custom           = in.custom_rules;
    const std::string ruleset = nft::build_nft_ruleset(ns);

    bool nft_ok = true;
    if (ruleset != m_last_ruleset) {
        std::string err;
        nft_ok = m_sinks.apply_nft
                 ? m_sinks.apply_nft(ruleset, &err)
                 : false;
        if (nft_ok) {
            m_last_ruleset = ruleset;
            ++m_apply_count;
            if (m_sinks.set_rules_applied_count)
                m_sinks.set_rules_applied_count(m_apply_count);
            if (m_sinks.set_last_apply_unix && in.now_unix)
                m_sinks.set_last_apply_unix(in.now_unix);
        }
    }

    // 2) Route metrics. Always run — apply_priorities is idempotent
    //    per-iface (ip route replace is a no-op when target == current).
    bool route_ok = true;
    if (m_sinks.apply_routes) {
        route_ok = m_sinks.apply_routes(in.ifaces_in_priority_order);
    }

    // 3) Emit net.iface.active if the pick changed (empty string when
    //    nothing is up — operator-visible signal that we're offline).
    std::string new_active;
    if (auto idx = iface::pick_active(in.ifaces_in_priority_order)) {
        new_active = in.ifaces_in_priority_order[*idx].name;
    }
    if (new_active != m_last_iface) {
        m_last_iface = new_active;
        if (m_sinks.set_iface_active) m_sinks.set_iface_active(new_active);
    }

    // 4) State: apply failure dominates; otherwise track the WAN uplink —
    //    NeedConfig while no iface is usable (waiting for an uplink),
    //    Steady once one is up.
    State next = (!nft_ok || !route_ok) ? State::Failed
               : new_active.empty()     ? State::NeedConfig
               :                          State::Steady;
    transition(next);
    return m_state;
}

} // namespace net_router
