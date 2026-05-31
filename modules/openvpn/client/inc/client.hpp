#ifndef __openvpn_client_client_hpp__
#define __openvpn_client_client_hpp__

/// Public-ish header for the openvpn-client module. Only really
/// "public" in the sense that the in-tree test target links against
/// the same lib (openvpn_client_lib) the binary uses — there is no
/// API stability contract for external consumers.
///
/// L12/D2 scaffold: holds the bare interface needed by main.cpp.
/// D3..D6 add DsBridge, mgmt protocol parser, process wrapper, and
/// the lifecycle FSM.

#include <cstdint>
#include <string>

namespace openvpn_client {

/// Status surfaced by the v0 main. Mirrors data_store::Status shape
/// so callers can switch on `.ok` + read `.err`.
struct Status {
    bool        ok = true;
    int         code = 0;
    std::string err;
};

/// Diagnostic mode: connect to ds-server, dump the vpn.* snapshot,
/// exit. Useful for bring-up to confirm DsBridge sees the right
/// values without spawning openvpn.
Status v0_dump_vpn_keys(const std::string& socketPath);

/// Full lifecycle: connect to ds-server, refuse on
/// missing_required(), spawn openvpn(8), connect to its management
/// socket, route STATE / PUSH_REPLY through Lifecycle::step into
/// DsBridge writes. Quiesces on subprocess exit (or after first
/// PUSH_REPLY if `once`). Writes `vpn.state=exited` +
/// `vpn.exit_code=<n>` before returning.
Status run_daemon(const std::string& socketPath,
                  const std::string& openvpn_path = "/usr/sbin/openvpn",
                  bool               once         = false);

} // namespace openvpn_client

#endif /* __openvpn_client_client_hpp__ */
