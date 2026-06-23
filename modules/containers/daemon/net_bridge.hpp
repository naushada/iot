#ifndef __iot_container_net_bridge_hpp__
#define __iot_container_net_bridge_hpp__

#include <string>

/// Bridge-mode container networking: the imperative `ip`/`nft` plumbing that
/// gives a container its own IP. The pure subnet/nft-ruleset logic is in
/// container_net (containers_core); this drives it via ACE_Process as root.
/// See apps/docs/tdd-device-containers.md (§ bridge mode).

namespace containers {

struct BridgeNet {
    bool        ok = false;
    std::string ip;        ///< IP assigned to the container (e.g. 10.88.0.2)
    std::string gateway;   ///< bridge gateway / container default route (.1)
    std::string error;
};

/// Bring up bridge networking for container `id` whose init is `container_pid`:
///   1. ensure the host bridge (iot-cni0) exists with the gateway IP + up,
///   2. enable IPv4 forwarding,
///   3. install the scoped `inet iot_containers` masquerade nft table,
///   4. create a veth pair, attach the host end to the bridge, move the peer
///      into the container netns as eth0 with the container IP + default route.
/// `subnet_cidr` is a /24 (default 10.88.0.0/24). Must run as root. On failure
/// the partial setup is best-effort torn down and `.error` is set.
BridgeNet bridge_up(long container_pid, const std::string& subnet_cidr,
                    const std::string& id);

/// Tear down container `id`'s veth (its peer auto-removes when the container
/// netns dies). Idempotent; the bridge + nft table persist for reuse.
void bridge_down(const std::string& id);

} // namespace containers

#endif /* __iot_container_net_bridge_hpp__ */
