#ifndef __iot_container_net_hpp__
#define __iot_container_net_hpp__

#include <string>

/// Pure container-networking helpers for the opt-in bridge mode: subnet
/// planning + the scoped nftables masquerade ruleset. No `ip`/`nft`/netlink, no
/// ACE — host-unit-testable. The actual veth/bridge/netns plumbing lives in the
/// daemon's net_bridge. See apps/docs/tdd-device-containers.md (§ bridge mode).

namespace containers {

/// Default bridge name + subnet for the single-container bridge network.
inline constexpr const char* kBridgeName    = "iot-cni0";
inline constexpr const char* kDefaultSubnet = "10.88.0.0/24";

struct NetPlan {
    bool        ok = false;
    std::string bridge;        ///< host bridge device
    std::string cidr;          ///< the subnet, e.g. "10.88.0.0/24"
    int         prefix = 0;    ///< prefix length (24)
    std::string gateway;       ///< bridge IP, the container's default route (.1)
    std::string container_ip;  ///< the container's address (.2)
};

/// Plan the single-container bridge network from `subnet_cidr` (a /24, e.g.
/// "10.88.0.0/24") + `bridge`. gateway is host .1, the container gets .2.
/// ok=false on a malformed or non-/24 CIDR (v1 supports a single /24).
NetPlan plan_bridge_net(const std::string& subnet_cidr, const std::string& bridge);

/// The nftables ruleset (an `nft -f` script) for container egress: a SCOPED
/// `inet iot_containers` table (separate from net-router's `iot_router`, so the
/// two never flush each other) with a postrouting masquerade for `cidr` leaving
/// any non-bridge interface, plus forward-accept for the bridge.
std::string nft_container_ruleset(const std::string& cidr, const std::string& bridge);

} // namespace containers

#endif /* __iot_container_net_hpp__ */
