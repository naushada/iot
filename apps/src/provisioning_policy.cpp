#include "provisioning_policy.hpp"

#include <nlohmann/json.hpp>

#include "psk_gen.hpp"

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

    // 1) Commissioned tier: a row whose sha256(serial)[:32] == presented.
    const auto arr =
        nlohmann::json::parse(credentials_json, /*cb*/nullptr,
                              /*allow_exceptions*/false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string serial = e.value("serial", std::string());
            const std::string key    = e.value("bs.psk.key", std::string());
            if (!serial.empty() && !key.empty() &&
                sha256_hex(serial).substr(0, 32) == presented)
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
    if (!master_hex.empty()) {
        const std::string serial = serial_from_dm_identity(presented);
        if (!serial.empty()) return derive_dm_psk_hex(master_hex, serial);
    }

    // 3) No match.
    return {};
}

} // namespace iot
