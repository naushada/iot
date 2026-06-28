#ifndef __iot_tenant_subnet_hpp__
#define __iot_tenant_subnet_hpp__

/// Multi-tenant VPN — pure subnet math + nftables isolation-rule generation
/// (apps/docs/tdd-multi-tenant-cloud.md, P3). No I/O, no nft/openvpn calls —
/// just the allocation + ruleset-text logic so it's unit-testable. The live
/// wiring (per-client CCD static IPs, applying the ruleset) is the caller's job
/// and needs a real tun/OpenVPN environment to validate.

#include <string>
#include <utility>
#include <vector>

namespace server { namespace openvpn {

/// True if the two CIDRs (e.g. "10.9.16.0/24") share any address. Invalid
/// inputs never overlap (return false).
bool subnets_overlap(const std::string& a, const std::string& b);

/// Carve the next free /`tenant_prefix` block out of `pool_cidr` (e.g. the
/// "10.9.0.0/16" tenant pool), skipping any block that overlaps an entry in
/// `used`. Returns the CIDR of the first free block, or "" when the pool is
/// exhausted or an input is malformed. `tenant_prefix` must be >= the pool
/// prefix and <= 30.
std::string allocate_tenant_subnet(const std::string& pool_cidr,
                                   const std::vector<std::string>& used,
                                   int tenant_prefix = 24);

/// Build an nftables ruleset (table `ip iot_tenant_isol`) that DROPS forwarded
/// traffic between *different* tenant subnets while leaving each tenant's
/// traffic to the cloud/services untouched (default-accept; we only add the
/// cross-tenant drops). Pairwise drops — explicit and order-independent.
/// Returns a full `table { ... }` block ready for `nft -f -`. Empty/one-tenant
/// input yields an empty table (nothing to isolate).
std::string build_tenant_isolation_rules(
    const std::vector<std::string>& tenant_subnets);

/// Reconcile a `cloud.tenants` JSON array: assign a fresh non-overlapping
/// /`tenant_prefix` block (carved from `pool_cidr`) to every tenant object that
/// lacks a non-empty "vpn.subnet", avoiding subnets already assigned to other
/// tenants. Returns {updated_json, changed}. A tenant left unassigned because
/// the pool is exhausted keeps no subnet (the caller logs it). On a non-array /
/// parse error returns {input, false}.
std::pair<std::string, bool> assign_missing_subnets(
    const std::string& tenants_json,
    const std::string& pool_cidr,
    int tenant_prefix = 24);

/// Per-tenant device quota (P5). True if provisioning `candidate_serial` into
/// `tenant` would exceed that tenant's "max.devices" (from `tenants_json`),
/// where the current count is the number of `creds_json` rows in the tenant.
/// A missing / <=0 max.devices is unlimited → false. Re-provisioning a serial
/// already in the tenant is always allowed → false (it doesn't grow the count).
bool tenant_at_capacity(const std::string& tenants_json,
                        const std::string& creds_json,
                        const std::string& tenant,
                        const std::string& candidate_serial);

// ── P3c: per-client static IPs in the tenant /24 (OpenVPN CCD) ───────────────

/// Allocate the next free host IP from `subnet_cidr` (e.g. a tenant's
/// "10.9.16.0/24"), skipping the network/gateway/broadcast addresses and any IP
/// in `used`. Gateway is `network+1`, so the first assignable host is
/// `network+2`. Returns the dotted IP, or "" when the subnet is full / invalid.
std::string allocate_ip_in_subnet(const std::string& subnet_cidr,
                                  const std::vector<std::string>& used);

/// Build an OpenVPN client-config-dir line pinning a client to `ip`:
///   "ifconfig-push <ip> <netmask>\n"
/// With `topology subnet`, the netmask is the *server* pool's mask (from
/// `server_cidr`, e.g. the "/16"), not the tenant /24 — the per-tenant boundary
/// is enforced by nftables (build_tenant_isolation_rules), not the mask.
/// Returns "" on invalid input.
std::string build_ccd_entry(const std::string& ip,
                            const std::string& server_cidr);

}} // namespace server::openvpn

#endif /* __iot_tenant_subnet_hpp__ */
