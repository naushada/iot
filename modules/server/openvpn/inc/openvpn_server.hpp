#ifndef __server_openvpn_openvpn_server_hpp__
#define __server_openvpn_openvpn_server_hpp__

/// Spawns and supervises an `openvpn(8)` process in **server** mode for
/// iot-cloudd. Sibling to VpnRegistry (which only allocates IPs/ports);
/// this actually runs the daemon.
///
/// Built on ACE_Process — same approach as the device-side
/// openvpn_client::OpenVpnProcess, but renders a *server* config
/// (mode server / dh none / server <subnet>) and exposes simple
/// start()/stop()/running()/pid() supervision that cloudd drives from
/// its main loop.

#include <chrono>
#include <ctime>         // std::time_t (parse_status_routing_table)
#include <map>
#include <string>
#include <sys/types.h>   // pid_t

namespace server {
namespace openvpn {

/// Inputs to build_server_config(). Plain POD — no data_store coupling,
/// so config rendering stays unit-testable. Cert/key paths and subnet
/// come from the cloud.vpn.* schema keys.
struct OpenVpnServerConfig {
    std::uint16_t port       = 1194;
    std::string   proto      = "udp";
    std::string   dev        = "tun";
    std::string   subnet     = "10.9.0.0/24";   // CIDR; → `server NET MASK`
    std::string   ca_path    = "/etc/iot/vpn/ca/ca.crt";
    std::string   cert_path  = "/etc/iot/vpn/server.crt";
    std::string   key_path   = "/etc/iot/vpn/server.key";
    std::string   cipher     = "AES-256-GCM";
    std::string   dns;                          // empty → no DNS pushed;
                                                // else `push "dhcp-option DNS …"`
    std::string   crl;                          // empty → no crl-verify; else
                                                // `crl-verify <path>` (revocation)
    std::uint16_t mgmt_port  = 7506;            // management 127.0.0.1 <port>
    int           verb       = 3;
};

/// Render a server-mode OpenVPN config. Pure. Uses `dh none` (ECDH), so
/// no dh.pem is required. Returns "" if the subnet CIDR is unparseable.
std::string build_server_config(const OpenVpnServerConfig& c);

/// Convert "10.9.0.0/24" → ("10.9.0.0", "255.255.255.0"). Returns false
/// on malformed input.
bool cidr_to_net_mask(const std::string& cidr,
                      std::string& net, std::string& mask);

/// One live tunnel as seen in the OpenVPN `status` ROUTING TABLE.
struct VpnRoute {
    std::string vip;       ///< virtual (tunnel) IP, e.g. 10.9.0.3
    std::string wan_ip;    ///< real/public (ISP) IP, :port stripped
};

/// Parse the ROUTING TABLE of an OpenVPN v1 `status` dump into
/// sanitized-serial → {vip, wan_ip}. Pure + unit-testable (no socket).
///
/// **Freshness filter (the "Launch UI while VPN down" fix).** A route whose
/// `Last Ref` is older than `max_age_secs` relative to `now` is DROPPED:
/// OpenVPN refreshes Last Ref on every packet (keepalive ping ~10s), so a live
/// client is always seconds-fresh, while a hard-powered-off device's route
/// freezes and ages out — long before OpenVPN's keepalive reaps it (2× the
/// restart value). Dropping stale routes makes the cloud's per-device "VPN up"
/// view (dev_tun_ip → Launch UI + DNAT) track reality in ~`max_age_secs`
/// instead of minutes. `max_age_secs <= 0` disables the filter.
///
/// **Fail-safe:** a Last Ref that can't be parsed KEEPS the entry — a format
/// surprise can never hide a genuinely-connected device (worst case reverts to
/// the pre-filter behavior). The CLIENT LIST section is ignored (its rows lead
/// with the real address, not the vip).
std::map<std::string, VpnRoute>
parse_status_routing_table(const std::string& status,
                           std::time_t now, int max_age_secs);

class OpenVpnServer {
public:
    explicit OpenVpnServer(OpenVpnServerConfig cfg,
                           std::string openvpn_path = "/usr/sbin/openvpn");
    ~OpenVpnServer();

    OpenVpnServer(const OpenVpnServer&)            = delete;
    OpenVpnServer& operator=(const OpenVpnServer&) = delete;

    /// Write the config to a temp file and spawn openvpn. No-op if already
    /// running. Returns true on successful fork+exec.
    bool start();

    /// Swap in a new config. If the rendered config is byte-identical to the
    /// current one, this is a no-op and returns false (so a redundant ds write
    /// can't bounce a healthy tunnel). On a real change it updates the config
    /// and stops any running child, returning true — the caller restarts it
    /// (e.g. via its supervisor) so the new config takes effect. Lets an
    /// operator change cloud.vpn.* (proto/port/cipher/…) live, no daemon restart.
    bool reconfigure(const OpenVpnServerConfig& cfg);

    /// SIGTERM (grace) then SIGKILL; reap. No-op if not running.
    void stop(std::chrono::milliseconds grace = std::chrono::seconds(3));

    /// Non-blocking liveness probe (waitpid WNOHANG). Reaps + clears pid on
    /// exit, caching the exit code.
    bool running();

    /// Child pid while running, else 0.
    pid_t pid() const { return m_pid; }

    /// Last observed exit code (after running() saw the child exit).
    int exit_code() const { return m_exit_code; }

    /// Tail of the last run's captured stdout+stderr (openvpn(8) writes its
    /// failure reason there). Empty if nothing was captured. Reads at most
    /// `max_bytes` from the end of the redirect file.
    std::string log_tail(std::size_t max_bytes = 4096) const;

private:
    OpenVpnServerConfig m_cfg;
    std::string         m_openvpn_path;
    std::string         m_config_path;   // temp file, unlinked on stop/dtor
    std::string         m_log_path;      // child stdout+stderr capture file
    pid_t               m_pid = 0;
    int                 m_exit_code = 0;
};

}  // namespace openvpn
}  // namespace server

#endif  // __server_openvpn_openvpn_server_hpp__
