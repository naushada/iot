#include "bootstrap.hpp"

namespace server {
namespace lwm2m {

BootstrapProvisioner::BootstrapProvisioner(EndpointRegistry& ep_reg,
                                           openvpn::VpnRegistry& vpn_reg)
    : m_ep_reg(ep_reg), m_vpn_reg(vpn_reg) {}

std::optional<BootstrapResult> BootstrapProvisioner::provision(
    const std::string& ep, const std::string& tenant_subnet) {

    const std::string server_uri = "coaps://cloud:5684";

    // Idempotent: an endpoint that already exists (re-provisioned to refresh its
    // BS PSK, or a watch replay after a cloudd restart / rehydrate) REUSES its
    // existing tunnel IP + proxy port instead of failing as a "duplicate". The
    // credential upsert is the meaningful work of a re-provision; failing here
    // logged a misleading "provision failed (dup or exhausted)" even though the
    // credential stored fine — and re-provisioning is exactly how an operator
    // restores a device's BS PSK after the cloud lost/rotated it.
    if (const EndpointInfo* existing = m_ep_reg.lookup_by_ep(ep)) {
        BootstrapResult result;
        result.endpoint            = ep;
        result.tun_ip              = existing->tun_ip;
        result.proxy_port          = existing->proxy_port;
        result.security_object_tlv = build_security_tlv(server_uri);
        result.server_object_tlv   = build_server_tlv(86400);
        return result;
    }

    // New endpoint: allocate tunnel IP + proxy port. A tenant device draws its
    // IP from its tenant /24 (P3c); the default tenant uses the base pool.
    auto alloc = tenant_subnet.empty()
                     ? m_vpn_reg.allocate(ep)
                     : m_vpn_reg.allocate_in_subnet(ep, tenant_subnet);
    if (!alloc.has_value()) return std::nullopt;   // subnet / ports exhausted

    // Register in the endpoint registry
    EndpointInfo info;
    info.ep         = ep;
    info.tun_ip     = alloc->tun_ip;
    info.proxy_port = alloc->proxy_port;
    info.registered = false;   // bootstrap first, register later
    if (!m_ep_reg.add(info)) {
        m_vpn_reg.release(ep);
        return std::nullopt;
    }

    BootstrapResult result;
    result.endpoint           = ep;
    result.tun_ip             = alloc->tun_ip;
    result.proxy_port         = alloc->proxy_port;
    result.security_object_tlv = build_security_tlv(server_uri);
    result.server_object_tlv   = build_server_tlv(86400);

    return result;
}

bool BootstrapProvisioner::deprovision(const std::string& ep) {
    if (!m_ep_reg.lookup_by_ep(ep)) return false;
    m_vpn_reg.release(ep);
    return m_ep_reg.remove(ep);
}

// ── Minimal TLV builders (OMA-TS-LightweightM2M Annex C) ──────────

namespace {

/// Write a single-resource TLV: Type byte + length + value.
/// Resource with value: type = 0b11000000 | identifier_length_bits.
/// We use 8-bit identifier + 8-bit length for simplicity.
void write_tlv_u8(std::string& out, std::uint8_t rid,
                  const std::string& value) {
    out.push_back(static_cast<char>(0b11000000 | 0b00001000)); // Resource, 8-bit ID
    out.push_back(static_cast<char>(rid));
    out.push_back(static_cast<char>(value.size()));
    out.append(value);
}

void write_tlv_bool(std::string& out, std::uint8_t rid, bool v) {
    out.push_back(static_cast<char>(0b11000000 | 0b00001000));
    out.push_back(static_cast<char>(rid));
    out.push_back(1);
    out.push_back(v ? '\x01' : '\x00');
}

void write_tlv_u32(std::string& out, std::uint8_t rid, std::uint32_t v) {
    out.push_back(static_cast<char>(0b11000000 | 0b00001000));
    out.push_back(static_cast<char>(rid));
    out.push_back(4);
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

} // namespace

std::string BootstrapProvisioner::build_security_tlv(
    const std::string& server_uri) {
    std::string tlv;
    // Security Object (OID 0), instance 0
    // RID 0: LwM2M Server URI
    write_tlv_u8(tlv, 0, server_uri);
    // RID 1: Bootstrap-Server flag = true
    write_tlv_bool(tlv, 1, true);
    // RID 2: Security Mode = 0 (Pre-Shared Key)
    write_tlv_u32(tlv, 2, 0);
    // RID 10: Short Server ID = 0 (bootstrap)
    write_tlv_u32(tlv, 10, 0);
    return tlv;
}

std::string BootstrapProvisioner::build_server_tlv(std::uint32_t lifetime) {
    std::string tlv;
    // Server Object (OID 1), instance 0
    // RID 0: Short Server ID = 1 (DM server)
    write_tlv_u32(tlv, 0, 1);
    // RID 1: Lifetime
    write_tlv_u32(tlv, 1, lifetime);
    // RID 7: Binding = "U"
    write_tlv_u8(tlv, 7, "U");
    return tlv;
}

} // namespace lwm2m
} // namespace server
