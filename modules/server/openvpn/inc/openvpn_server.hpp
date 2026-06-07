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

    /// SIGTERM (grace) then SIGKILL; reap. No-op if not running.
    void stop(std::chrono::milliseconds grace = std::chrono::seconds(3));

    /// Non-blocking liveness probe (waitpid WNOHANG). Reaps + clears pid on
    /// exit, caching the exit code.
    bool running();

    /// Child pid while running, else 0.
    pid_t pid() const { return m_pid; }

    /// Last observed exit code (after running() saw the child exit).
    int exit_code() const { return m_exit_code; }

private:
    OpenVpnServerConfig m_cfg;
    std::string         m_openvpn_path;
    std::string         m_config_path;   // temp file, unlinked on stop/dtor
    pid_t               m_pid = 0;
    int                 m_exit_code = 0;
};

}  // namespace openvpn
}  // namespace server

#endif  // __server_openvpn_openvpn_server_hpp__
