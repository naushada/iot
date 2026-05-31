#ifndef __net_router_shell_hpp__
#define __net_router_shell_hpp__

/// Tiny shell-runner abstraction used by iface_monitor and ip_route.
/// The "real" runner shells out via popen(); tests inject a fake
/// runner so they never touch the host's network stack.

#include <functional>
#include <string>
#include <vector>

namespace net_router::shell {

/// Args are passed positionally — argv[0] is the binary. Caller does
/// NOT shell-escape; the default runner builds a safely-quoted command
/// line internally.
///
/// Returns stdout of the child. Non-zero exit + stderr go through the
/// `exit_code` outparam if provided (nullptr → ignored).
using Runner = std::function<std::string(const std::vector<std::string>& argv,
                                         int* exit_code)>;

/// popen()-backed runner. Builds `"<arg0>" "<arg1>" ... 2>/dev/null`,
/// pipes stdout back, writes `WEXITSTATUS` to `*exit_code`.
Runner default_runner();

} // namespace net_router::shell

#endif /* __net_router_shell_hpp__ */
