#ifndef __iot_tenant_policy_hpp__
#define __iot_tenant_policy_hpp__

/// Multi-tenant cloud — P1 core primitives (pure logic, no ds/ACE/sockets).
///
/// One cloud-iot deployment hosts N tenants. These helpers are the shared
/// foundation the device plane (BS/DM resolvers) and the console (read
/// filtering) build on: tenant-id validation, endpoint<->(tenant,serial)
/// splitting, tenant-qualified PSK identities, per-row tenant tagging, and
/// cloud.tenants registry lookup. Everything is backward compatible: the
/// reserved "default" tenant reproduces today's single-tenant behaviour
/// byte-for-byte, so fielded devices and existing deployments are unaffected.
///
/// See apps/docs/tdd-multi-tenant-cloud.md.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iot {

/// The implicit tenant for untagged rows / colon-less endpoints / legacy
/// identities. Its identity scheme is the pre-multi-tenant one.
inline constexpr const char* kDefaultTenant = "default";

/// Valid tenant slug: 1..32 chars of [a-z0-9-], not starting or ending with
/// '-'. "default" is valid. "*" (the platform-operator pseudo-tenant) is NOT a
/// valid stored tenant id.
bool valid_tenant_id(const std::string& id);

/// (tenant, serial) pair parsed from a bootstrap endpoint.
struct EndpointId {
    std::string tenant;
    std::string serial;
    bool operator==(const EndpointId& o) const {
        return tenant == o.tenant && serial == o.serial;
    }
};

/// Split a bootstrap endpoint ("ep" query) into (tenant, serial):
///   "acme:000000ff" -> {"acme", "000000ff"}
///   "000000ff"      -> {"default", "000000ff"}   (no colon = legacy)
/// A malformed prefix (invalid tenant slug, or empty serial after the colon)
/// falls back to {"default", <whole ep>} — a bad prefix must never silently
/// drop a device into the wrong tenant.
EndpointId split_endpoint(const std::string& ep);

/// Inverse of split_endpoint: default tenant -> serial unchanged (legacy on the
/// wire); any other tenant -> "tenant:serial".
std::string join_endpoint(const std::string& tenant, const std::string& serial);

/// Commissioned-tier BS DTLS PSK identity (32 hex chars / 128-bit, fits
/// DTLS_PSK_MAX_KEY_LEN). default -> sha256(serial)[:32] (legacy, unchanged);
/// any other tenant -> sha256("tenant:serial")[:32]. Distinct tenants with the
/// same serial therefore present distinct identities (no collision).
std::string bs_identity(const std::string& tenant, const std::string& serial);

/// DM PSK identity. default -> "rpi<serial>@cloud.local" (legacy); any other
/// tenant -> "rpi<serial>@<tenant>.cloud.local".
std::string dm_identity(const std::string& tenant, const std::string& serial);

/// Parse a DM identity back into (tenant, serial). The legacy
/// "rpi<serial>@cloud.local" -> {"default", serial}; the multi-tenant
/// "rpi<serial>@<tenant>.cloud.local" -> {tenant, serial}. On any malformed
/// input returns {"", ""} (both empty) so the caller can reject it.
EndpointId parse_dm_identity(const std::string& identity);

/// The tenant a credential/endpoint row belongs to: the row object's "tenant"
/// string, or "default" when absent/empty/unparseable. `row_json` is one JSON
/// object.
std::string row_tenant(const std::string& row_json);

/// Filter a JSON array string to only the rows whose tenant == `tenant`.
/// Returns a JSON array string; "[]" on parse error or no match. Used by the
/// console to scope cloud.endpoints / cloud.endpoint.credentials reads.
std::string filter_rows_by_tenant(const std::string& array_json,
                                  const std::string& tenant);

/// One tenant record from the cloud.tenants registry.
struct TenantInfo {
    std::string   id;
    std::string   name;
    std::string   vpn_subnet;     ///< e.g. "10.9.16.0/24"
    std::string   dm_uri;         ///< e.g. "coaps://host:5683"
    std::uint16_t proxy_start{0};
    std::uint16_t proxy_end{0};
    bool          active{true};
};

/// Look up a tenant by id in a cloud.tenants JSON array string. nullopt if not
/// found / parse error.
std::optional<TenantInfo> find_tenant(const std::string& tenants_json,
                                      const std::string& id);

/// All tenant ids in declaration order ("[]"/parse error -> empty).
std::vector<std::string> list_tenant_ids(const std::string& tenants_json);

} // namespace iot

#endif /* __iot_tenant_policy_hpp__ */
