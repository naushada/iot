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
            // Device-agnostic tenancy (Option B): the device presents the SAME
            // bare canonical identity regardless of tenant — sha256(serial)[:32].
            // Serials are globally unique across tenants, so this is unambiguous;
            // the tenant lives only in the matched row's "tenant" tag (read
            // downstream, not needed for key selection). Identical to the
            // single-tenant behaviour, so fielded devices are unaffected.
            if (!serial.empty() && !key.empty() &&
                sha256_hex(serial).substr(0, 32) == presented)
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
    // Device-agnostic tenancy (Option B): the device bootstraps with its bare
    // serial — it never sends its tenant. The endpoint IS the serial; the tenant
    // is the matched credential row's "tenant" tag (serials are globally unique,
    // so the match is unambiguous). Identities are bare (rpi<serial>@cloud.local,
    // sha256(serial)[:32]), exactly as single-tenant. The tenant only selects the
    // per-tenant dm.uri + tags the endpoint downstream.
    BsAccountPlan p;
    p.serial = ep;
    p.tenant = kDefaultTenant;
    p.dm_uri = global_dm_uri;

    // Commissioned row matched by serial (or the legacy identity==ep override).
    auto arr = nlohmann::json::parse(credentials_json, nullptr, false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            if (e.value("serial", std::string()) != ep &&
                e.value("identity", std::string()) != ep)
                continue;
            const std::string rt = e.value("tenant", std::string());
            p.tenant      = rt.empty() ? std::string(kDefaultTenant) : rt;
            p.bs_key      = e.value("bs.psk.key", std::string());
            p.dm_key      = e.value("dm.psk.key", std::string());
            p.dm_identity = e.value("dm.psk.id",  std::string());
            break;
        }
    }

    // A non-default tenant must be a known, active tenant; its dm.uri (when set)
    // overrides the global one. A default deployment has no cloud.tenants.
    {
        auto t = find_tenant(tenants_json, p.tenant);
        if (p.tenant != kDefaultTenant && (!t.has_value() || !t->active))
            return p;                                        // ok stays false
        if (t.has_value() && !t->dm_uri.empty()) p.dm_uri = t->dm_uri;
    }

    // Zero-touch (HKDF) tier: no stored row but a master is available → derive
    // statelessly from the bare serial.
    if ((p.dm_key.empty() || p.dm_identity.empty()) && !master_hex.empty()) {
        p.bs_key      = derive_bs_psk_hex(master_hex, p.serial);
        p.dm_key      = derive_dm_psk_hex(master_hex, p.serial);
        p.dm_identity = format_dm_identity(p.serial);        // bare
        p.zero_touch  = true;
    }

    if (p.dm_identity.empty()) p.dm_identity = format_dm_identity(p.serial);

    // BS DTLS identity written to the device's Security /0/0 — the bare canonical
    // sha256(serial)[:32] (zero-touch keeps the raw serial the device presents).
    p.bs_identity = p.zero_touch ? p.serial
                                 : sha256_hex(p.serial).substr(0, 32);

    p.ok = !p.dm_identity.empty() && !p.dm_key.empty() && !p.dm_uri.empty();
    return p;
}

} // namespace iot
