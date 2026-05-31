#ifndef __net_router_lifecycle_hpp__
#define __net_router_lifecycle_hpp__

/// Lifecycle FSM that drives net-router's reactor tick.
///
/// Pure logic: takes a Sinks struct (apply-nft, apply-routes, ds
/// writers) and an Inputs snapshot (ds-server reads + iface probe
/// results + current unix time) and computes the next action set.
/// Mirrors modules/openvpn/client/src/lifecycle.hpp's pattern so
/// tests can substitute lambdas without standing up a real DsBridge,
/// iface_monitor, nft, or ds-server.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "iface_monitor.hpp"
#include "nft_rules.hpp"

namespace net_router {

class Lifecycle {
public:
    enum class State {
        Init,         ///< Constructed; nothing applied yet
        NeedConfig,   ///< Required keys (net.lwm2m.target_ip) absent
        Steady,       ///< Last apply succeeded
        Failed,       ///< Last apply (nft or routes) returned false
    };

    /// Per-action writers. Production wires apply_nft to
    /// apply::default_nft_apply(), apply_routes to a closure over
    /// route::apply_priorities(), and the set_* writers to the
    /// DsBridge setters. Tests substitute lambdas that capture.
    struct Sinks {
        std::function<bool(const std::string& ruleset,
                           std::string*       err)>            apply_nft;
        std::function<bool(const std::vector<iface::State>&)>  apply_routes;
        std::function<void(const std::string&)>                set_state;
        std::function<void(const std::string&)>                set_iface_active;
        std::function<void(std::uint32_t)>                     set_rules_applied_count;
        std::function<void(std::uint32_t)>                     set_last_apply_unix;
    };

    /// Snapshot the Lifecycle uses to compute its next step.
    struct Inputs {
        // From DsBridge — net.* read keys
        std::string                tun_dev;
        std::string                lwm2m_target_ip;
        std::uint32_t              lwm2m_target_port = 0;
        std::vector<std::uint16_t> forward_ports;
        std::vector<nft::CustomRule> custom_rules;
        // From iface_monitor::probe_all() in priority order
        std::vector<iface::State>  ifaces_in_priority_order;
        // Wall-clock (passed in for determinism in tests)
        std::uint32_t              now_unix = 0;
    };

    explicit Lifecycle(Sinks sinks) : m_sinks(std::move(sinks)) {}

    /// One reactor tick. Returns the new State.
    /// - If lwm2m_target_ip empty → NeedConfig, no side effects
    /// - Else build ruleset; if differs from last → apply_nft
    /// - Pick active iface; apply_routes; if changed → set_iface_active
    /// - On any nft/route failure → Failed (kept until next clean tick)
    /// - On all-ok → Steady (and emit set_state("steady") if changed)
    State step(const Inputs& in);

    State state()                       const { return m_state;          }
    std::uint32_t apply_count()         const { return m_apply_count;    }
    const std::string& last_ruleset()   const { return m_last_ruleset;   }
    const std::string& last_iface()     const { return m_last_iface;     }

    /// State → schema-friendly string for set_state ("init",
    /// "need-config", "steady", "failed").
    static const char* state_name(State s);

private:
    Sinks         m_sinks;
    State         m_state        = State::Init;
    std::uint32_t m_apply_count  = 0;
    std::string   m_last_ruleset;
    std::string   m_last_iface;

    void transition(State next);
};

} // namespace net_router

#endif /* __net_router_lifecycle_hpp__ */
