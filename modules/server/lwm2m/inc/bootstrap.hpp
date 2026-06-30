#ifndef __server_lwm2m_bootstrap_hpp__
#define __server_lwm2m_bootstrap_hpp__

/// L21/D3 — LwM2M Bootstrap Server provisioner.
///
/// Provisions a new device: assigns tunnel IP + proxy port via the
/// VPN registry, registers it in the endpoint registry, and builds
/// Security Object (OID 0) + Server Object (OID 1) binary TLV
/// payloads for the CoAP /bs handshake.

#include "endpoint_registry.hpp"
#include "vpn_registry.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace server {
namespace lwm2m {

struct BootstrapResult {
    std::string endpoint;
    std::string tun_ip;
    std::uint16_t proxy_port = 0;
    std::string security_object_tlv;  // OID 0 binary TLV
    std::string server_object_tlv;    // OID 1 binary TLV
};

class BootstrapProvisioner {
public:
    BootstrapProvisioner(EndpointRegistry& ep_reg, openvpn::VpnRegistry& vpn_reg);

    /// Provision a new endpoint.  Returns nullopt when the subnet/port pool is
    /// exhausted (an already-provisioned endpoint reuses its allocation).
    ///
    /// Multi-tenant (P3c): when `tenant_subnet` is non-empty (the device's
    /// tenant /24, e.g. "10.9.16.0/24"), the tunnel IP is allocated from THAT
    /// subnet via VpnRegistry::allocate_in_subnet instead of the base pool, so
    /// the device lands in its tenant's address range (pinned by an OpenVPN CCD
    /// file). Empty ⇒ the legacy base-pool allocation (default tenant).
    std::optional<BootstrapResult> provision(const std::string& ep,
                                             const std::string& tenant_subnet = "");

    /// Remove a previously provisioned endpoint.
    bool deprovision(const std::string& ep);

private:
    EndpointRegistry& m_ep_reg;
    openvpn::VpnRegistry& m_vpn_reg;

    /// Build a minimal Security Object (OID 0) binary TLV.
    /// RID 0: coaps://<server_ip>:5684
    /// RID 1: true (bootstrap server)
    /// RID 10: short server ID = 0
    static std::string build_security_tlv(const std::string& server_uri);

    /// Build a minimal Server Object (OID 1) binary TLV.
    /// RID 0: Short Server ID = 1
    /// RID 1: Lifetime = 86400
    /// RID 7: Binding = "U"
    static std::string build_server_tlv(std::uint32_t lifetime);
};

} // namespace lwm2m
} // namespace server

#endif
