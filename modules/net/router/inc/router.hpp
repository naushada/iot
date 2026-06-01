#ifndef __net_router_router_hpp__
#define __net_router_router_hpp__

/// Public-ish header for the net-router module. Same shape as
/// `modules/openvpn/client/inc/client.hpp` — only "public" because
/// the test binary links the same lib the daemon uses.
///
/// L13/D2 scaffold: holds the bare interface needed by main.cpp.
/// D3..D7 add DsBridge, nft_rules, ip_route, iface_monitor, apply,
/// + packaging integration.

#include <string>

namespace net_router {

/// Mirrors the data_store::Status / openvpn_client::Status shape.
struct Status {
    bool        ok = true;
    int         code = 0;
    std::string err;
};

/// Diagnostic mode: connect to ds-server, dump the net.* snapshot,
/// exit. Useful for bring-up to confirm the schema landed + the
/// daemon would see the right values without spawning anything
/// privileged.
Status v0_dump_net_keys(const std::string& socketPath);

/// Daemon mode: connect to ds-server, build a Lifecycle wiring
/// nft apply + ip route + DsBridge writers, then tick every
/// `poll_interval_sec_override` seconds (0 = use net.poll.interval.sec
/// from ds, falls back to 5s). Returns when SIGTERM/SIGINT fires
/// (g_run flag flipped by signal handler) or a fatal error happens.
Status run_daemon(const std::string& socketPath,
                  const std::string& nft_path,
                  unsigned           poll_interval_sec_override = 0);

} // namespace net_router

#endif /* __net_router_router_hpp__ */
