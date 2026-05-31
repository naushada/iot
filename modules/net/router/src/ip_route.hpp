#ifndef __net_router_ip_route_hpp__
#define __net_router_ip_route_hpp__

/// Wraps `ip route replace default …` so the daemon can express
/// outgoing-traffic preference as: eth=100, wifi=200, cellular=300
/// (lower metric wins in the Linux FIB). Pure-ish: every shell call
/// goes through the injected `shell::Runner`.

#include <cstdint>
#include <string>
#include <vector>

#include "iface_monitor.hpp"
#include "shell.hpp"

namespace net_router::route {

/// One step the apply produced. `cmd` is the argv that was (or would
/// have been) handed to the runner. `applied=false` means the step
/// was deliberately skipped (iface not up, no gateway, etc.); `error`
/// holds the reason in that case.
struct Step {
    std::string              iface;
    std::uint32_t            metric  = 0;
    std::vector<std::string> cmd;
    bool                     applied = false;
    int                      rc      = 0;
    std::string              error;
};

struct ApplyResult {
    std::vector<Step> steps;
    bool              all_ok = true;
};

/// Write default-route metrics for the priority-ordered iface list.
/// `metric = (index + 1) * 100` → first iface gets 100, second 200, …
/// Skips ifaces that aren't up or have no gateway. A runner that
/// returns non-zero `exit_code` marks the step failed and flips
/// `ApplyResult::all_ok` to false but does NOT abort — every iface
/// is attempted independently so a busted wifi doesn't block eth.
ApplyResult apply_priorities(const std::vector<iface::State>& priority_ordered,
                             shell::Runner runner = shell::default_runner());

} // namespace net_router::route

#endif /* __net_router_ip_route_hpp__ */
