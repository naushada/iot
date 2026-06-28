#include "provisioning_policy.hpp"

#include <nlohmann/json.hpp>

#include "psk_gen.hpp"
#include "tenant_policy.hpp"   // multi-tenant: bs_identity / parse_dm_identity

namespace iot {

EndpointResolution resolve_endpoint(
    const std::string&                cli_ep,
    const std::optional<std::string>& ds_serial,
    bool                              is_rpi,
    const std::string&                detected_serial) {

    // 1) Explicit CLI override always wins; never auto-writes a serial.
    if (!cli_ep.empty())
        return {cli_ep, /*serial_to_write*/"", /*ready*/true};

    // 2) A serial already in the data-store (installer-entered on non-RPi,
    //    or a prior RPi auto-fill) is authoritative.
    if (ds_serial && !ds_serial->empty())
        return {*ds_serial, "", true};

    // 3) RPi with a freshly detected serial → use it and signal that the
    //    caller should persist it to iot.serial (auto-fill).
    if (is_rpi && !detected_serial.empty())
        return {detected_serial, detected_serial, true};

    // 4) Non-RPi with nothing provisioned yet → defer registration.
    return {"", "", false};
}

bool should_restart_on_psk_change(bool               initialized,
                                  const std::string& loaded,
                                  const std::string& observed) {
    return initialized && observed != loaded;
}

bool is_coap_client_error(std::uint8_t coap_code) {
    return (coap_code >> 5) == 4;
}

bool should_rebootstrap(bool dm_dtls_failed, bool dm_registration_rejected) {
    return dm_dtls_failed || dm_registration_rejected;
}

std::string resolve_bs_psk(const std::string& credentials_json,
                           const std::string& presented,
                           const std::string& master_hex) {
    if (presented.empty()) return {};

    // 1) Commissioned tier: a row whose sha256(serial)[:32] == presented (the
    //    canonical BS identity the device derives by default), OR — fallback —
    //    a row whose formatted identity / dm.psk.id == presented. The fallback
    //    covers devices that present their DM-style identity at the BS handshake
    //    (iot.bs.psk.override=true with iot.bs.psk.identity=rpi<serial>@cloud.
    //    local). Either form returns the same bs.psk.key, so a device whose
    //    iot.bs.psk.key matches the commissioned row authenticates regardless of
    //    which identity convention it sends. sha256 is tried first so the
    //    canonical path is unchanged.
    const auto arr =
        nlohmann::json::parse(credentials_json, /*cb*/nullptr,
                              /*allow_exceptions*/false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string serial = e.value("serial", std::string());
            const std::string key    = e.value("bs.psk.key", std::string());
            // Tenant-aware canonical BS identity. For an untagged row this is
            // bs_identity("default", serial) == sha256(serial)[:32] — byte-for-
            // byte the legacy match, so fielded devices are unaffected. A
            // tenant-tagged row matches only its tenant's identity, so two
            // tenants sharing a serial never cross-authenticate.
            const std::string tenant = e.value("tenant", std::string());
            if (!serial.empty() && !key.empty() &&
                bs_identity(tenant, serial) == presented)
                return key;
        }
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string key = e.value("bs.psk.key", std::string());
            if (key.empty()) continue;
            if (e.value("identity", std::string())  == presented ||
                e.value("dm.psk.id", std::string()) == presented)
                return key;
        }
    }

    // 2) Zero-touch tier: derive from the presented raw serial (verbatim).
    if (!master_hex.empty())
        return derive_bs_psk_hex(master_hex, presented);

    // 3) No match — handshake fails.
    return {};
}

bool should_mint_dm(const std::string& credentials_json,
                    const std::string& serial) {
    if (serial.empty()) return false;
    const auto arr =
        nlohmann::json::parse(credentials_json, /*cb*/nullptr,
                              /*allow_exceptions*/false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (e.is_object() && e.value("serial", std::string()) == serial)
                return false;   // already provisioned → reuse, don't re-mint
        }
    }
    return true;                // no row → mint
}

namespace {
const char kDmPrefix[] = "rpi";
const char kDmSuffix[] = "@cloud.local";
} // namespace

std::string format_dm_identity(const std::string& serial) {
    return std::string(kDmPrefix) + serial + kDmSuffix;
}

std::string serial_from_dm_identity(const std::string& identity) {
    const std::size_t plen = sizeof(kDmPrefix) - 1;
    const std::size_t slen = sizeof(kDmSuffix) - 1;
    if (identity.size() <= plen + slen) return {};
    if (identity.compare(0, plen, kDmPrefix) != 0) return {};
    if (identity.compare(identity.size() - slen, slen, kDmSuffix) != 0)
        return {};
    return identity.substr(plen, identity.size() - plen - slen);
}

std::string resolve_dm_psk(const std::string& credentials_json,
                           const std::string& presented,
                           const std::string& master_hex) {
    if (presented.empty()) return {};

    // 1) Commissioned tier: a row whose dm.psk.id == presented.
    const auto arr =
        nlohmann::json::parse(credentials_json, /*cb*/nullptr,
                              /*allow_exceptions*/false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            if (e.value("dm.psk.id", std::string()) == presented) {
                const std::string key = e.value("dm.psk.key", std::string());
                if (!key.empty()) return key;
            }
        }
    }

    // 2) Zero-touch tier: derive from the serial embedded in the DM identity.
    // parse_dm_identity handles BOTH the legacy "rpi<serial>@cloud.local" and
    // the tenant "rpi<serial>@<tenant>.cloud.local" forms, so a default-tenant
    // device derives exactly as before.
    if (!master_hex.empty()) {
        const std::string serial = parse_dm_identity(presented).serial;
        if (!serial.empty()) return derive_dm_psk_hex(master_hex, serial);
    }

    // 3) No match.
    return {};
}

BsAccountPlan plan_bs_account(const std::string& ep,
                              const std::string& credentials_json,
                              const std::string& tenants_json,
                              const std::string& global_dm_uri,
                              const std::string& master_hex) {
    BsAccountPlan p;
    const EndpointId eid = split_endpoint(ep);
    p.tenant = eid.tenant;
    p.serial = eid.serial;

    // A non-default tenant must be a known, active tenant. The default tenant is
    // always allowed (legacy single-tenant deployments have no cloud.tenants).
    p.dm_uri = global_dm_uri;
    if (p.tenant != kDefaultTenant) {
        auto t = find_tenant(tenants_json, p.tenant);
        if (!t.has_value() || !t->active) return p;          // ok stays false
        if (!t->dm_uri.empty()) p.dm_uri = t->dm_uri;        // per-tenant override
    } else {
        // The default tenant may still carry a registry override.
        auto t = find_tenant(tenants_json, p.tenant);
        if (t.has_value() && !t->dm_uri.empty()) p.dm_uri = t->dm_uri;
    }

    // Commissioned row, scoped to this tenant.
    auto arr = nlohmann::json::parse(credentials_json, nullptr, false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string rt = e.value("tenant", std::string());
            const std::string rtenant = rt.empty() ? std::string(kDefaultTenant) : rt;
            if (rtenant != p.tenant) continue;
            const std::string rserial = e.value("serial", std::string());
            // Match by serial within the tenant; for the default tenant also
            // honour the legacy identity==ep override path.
            const bool match =
                (rserial == p.serial) ||
                (p.tenant == kDefaultTenant &&
                 e.value("identity", std::string()) == ep);
            if (!match) continue;
            p.bs_key      = e.value("bs.psk.key", std::string());
            p.dm_key      = e.value("dm.psk.key", std::string());
            p.dm_identity = e.value("dm.psk.id",  std::string());
            break;
        }
    }

    // Zero-touch (HKDF) tier: no stored row but a master is available → derive
    // statelessly. The device presents its raw serial as the BS identity here
    // (override path), matching the legacy zero-touch behaviour.
    if ((p.dm_key.empty() || p.dm_identity.empty()) && !master_hex.empty()) {
        p.bs_key      = derive_bs_psk_hex(master_hex, p.serial);
        p.dm_key      = derive_dm_psk_hex(master_hex, p.serial);
        p.dm_identity = dm_identity(p.tenant, p.serial);
        p.zero_touch  = true;
    }

    if (p.dm_identity.empty())
        p.dm_identity = dm_identity(p.tenant, p.serial);

    // BS DTLS identity written to the device's Security /0/0. Commissioned tier
    // uses the tenant-qualified canonical identity (default ⇒ sha256(serial)[:32]
    // byte-for-byte); zero-touch keeps the raw serial the device presents.
    p.bs_identity = p.zero_touch ? p.serial : bs_identity(p.tenant, p.serial);

    p.ok = !p.dm_identity.empty() && !p.dm_key.empty() && !p.dm_uri.empty();
    return p;
}

} // namespace iot
