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

/// v0 entry point: connect to ds-server at `socketPath` (empty →
/// default `/var/run/iot/data_store.sock`), `get` the known vpn.*
/// keys, dump them to stderr (ACE logging), exit. No openvpn
/// subprocess yet — D5+D6 add it.
///
/// Returns Status{ok=true} on success even when a key is unset
/// (an unset key is just absent from the output). ok=false on
/// connect failure or wire-level error.
Status v0_dump_vpn_keys(const std::string& socketPath);

} // namespace openvpn_client

#endif /* __openvpn_client_client_hpp__ */
