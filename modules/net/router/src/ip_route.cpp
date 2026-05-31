#include "ip_route.hpp"

namespace net_router::route {

ApplyResult apply_priorities(const std::vector<iface::State>& priority_ordered,
                             shell::Runner runner) {
    ApplyResult out;
    for (std::size_t i = 0; i < priority_ordered.size(); ++i) {
        const auto& s = priority_ordered[i];
        Step step;
        step.iface  = s.name;
        step.metric = static_cast<std::uint32_t>((i + 1) * 100);

        if (s.name.empty()) {
            step.error = "iface name empty";
            out.all_ok = false;
            out.steps.push_back(std::move(step));
            continue;
        }
        if (!s.up) {
            step.error = "iface not up";
            out.steps.push_back(std::move(step));
            continue;                          // not an error, just skipped
        }
        if (s.gateway.empty()) {
            step.error = "no default gateway known";
            out.steps.push_back(std::move(step));
            continue;
        }

        step.cmd = {
            "ip", "route", "replace", "default",
            "via", s.gateway,
            "dev", s.name,
            "metric", std::to_string(step.metric),
        };
        if (!runner) {
            step.error = "no shell runner";
            out.all_ok = false;
            out.steps.push_back(std::move(step));
            continue;
        }
        int rc = 0;
        runner(step.cmd, &rc);
        step.rc      = rc;
        step.applied = (rc == 0);
        if (rc != 0) {
            step.error = "ip route replace returned non-zero";
            out.all_ok = false;
        }
        out.steps.push_back(std::move(step));
    }
    return out;
}

} // namespace net_router::route
