#ifndef __server_dnat_device_dnat_hpp__
#define __server_dnat_device_dnat_hpp__

/// Per-device "device UI over VPN" DNAT rule generator + applier.
///
/// An operator reaches a device's local web UI over the VPN by hitting
/// the cloud on the device's assigned proxy port. The mechanism is
/// nftables DNAT (NOT an application-layer reverse proxy):
///
///     cloud:<proxy_port>  →  DNAT  →  <tun_ip>:<ui_port>   over tun0
///
/// per device. This lives in iot-cloudd because iot-cloudd runs the
/// OpenVPN *server*, so tun0 (and CAP_NET_ADMIN) are in its netns —
/// the only place the DNAT can route over the tunnel.
///
/// build_device_dnat_ruleset() is pure (no syscalls, no nft, no logging)
/// so it stays unit-testable; apply_ruleset() is the privileged step that
/// pipes the script to `nft -f -` (mirrors how iot-cloudd shells out for
/// openvpn). The generated script is scoped to its own table
/// (`ip iot_cloud_dnat`) and is fully rebuildable — it always begins by
/// flushing that table, so re-applying never doubles and a daemon restart
/// restores all rules atomically from cloud.endpoints.

#include <cstdint>
#include <string>
#include <vector>

namespace server {
namespace dnat {

/// One provisioned device's forwarding mapping.
struct DeviceForward {
    std::string   tun_ip;          ///< device VPN virtual IP (e.g. 10.9.0.12)
    std::uint16_t proxy_port = 0;  ///< per-device cloud port (5001-6000)
};

/// Inputs to the ruleset generator. Plain POD so the tests stay pure.
struct RulesetInput {
    std::string                tun_dev = "tun0";  ///< openvpn tun device
    std::uint16_t              ui_port = 80;       ///< device UI port (global)
    std::vector<DeviceForward> devices;            ///< one entry per device
};

/// Render the full nft script text. Idempotent + rebuildable: always
/// starts with `flush table ip iot_cloud_dnat` so re-applying never
/// doubles, and a restart reconstructs the entire rule set from the
/// current endpoint list. Skips devices missing a tun_ip or proxy_port.
std::string build_device_dnat_ruleset(const RulesetInput& in);

/// Render the human-readable mapping for one device, e.g.
/// "tcp dport 5001 -> 10.9.0.12:80". Used by logging; the UI renders
/// the same shape client-side.
std::string format_mapping(const DeviceForward& d, std::uint16_t ui_port);

/// Pipe `script` to `nft -f -`. Returns true on a clean (rc==0) apply.
/// Shells out (mirrors the openvpn supervisor). On failure the caller
/// keeps the previous live ruleset (this never partially applies because
/// `nft -f -` is atomic per file).
bool apply_ruleset(const std::string& script,
                   const std::string& nft_path = "nft");

/// Enable IPv4 forwarding (net.ipv4.ip_forward=1) so DNAT'd connections
/// route out over the tunnel. Best-effort; returns false if the sysctl
/// can't be written (logged by the caller).
bool enable_ip_forward();

} // namespace dnat
} // namespace server

#endif /* __server_dnat_device_dnat_hpp__ */
