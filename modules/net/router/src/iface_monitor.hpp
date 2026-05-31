#ifndef __net_router_iface_monitor_hpp__
#define __net_router_iface_monitor_hpp__

/// Stateless prober for a Linux network interface — shells out to
/// `ip -j link show <name>` (oper state) and `ip -j route show
/// default dev <name>` (gateway). All shell calls go through a
/// `shell::Runner` so tests can inject canned JSON without touching
/// the host.

#include <optional>
#include <string>
#include <vector>

#include "shell.hpp"

namespace net_router::iface {

struct State {
    std::string name;       ///< e.g. "eth0"
    bool        present = false;  ///< iface exists in the kernel
    bool        up      = false;  ///< OPER state == "UP"
    std::string gateway;    ///< default route's `via`, empty if none
};

/// Probe a single interface. Empty `name` → returns a zero-init State
/// (present=false). Parse failures are silent — the resulting State
/// just keeps the defaults so the caller treats it as "not usable".
State probe(const std::string& name,
            shell::Runner       runner = shell::default_runner());

/// Probe a list of interfaces in order.
std::vector<State> probe_all(const std::vector<std::string>& names,
                             shell::Runner runner = shell::default_runner());

/// Pick the highest-priority interface that is BOTH present and up.
/// Returns the index into `states` or std::nullopt if none qualify.
std::optional<std::size_t> pick_active(const std::vector<State>& states);

} // namespace net_router::iface

#endif /* __net_router_iface_monitor_hpp__ */
