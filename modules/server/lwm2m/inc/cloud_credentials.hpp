#ifndef __server_lwm2m_cloud_credentials_hpp__
#define __server_lwm2m_cloud_credentials_hpp__

/// PSK provisioning (task M) — cloud per-endpoint credential helpers.
///
/// The cloud owns the formatted identity namespace `rpi<serial>@cloud.local`
/// which keys both the BS and DM PSKs. Provisioning stores credentials as a
/// JSON array under `cloud.endpoint.credentials`; the BS/DM servers load it
/// live. These pure helpers do the formatting + array upsert/remove so the
/// logic is unit-testable away from the data-store + reactor.
///
/// See apps/docs/tdd-psk-provisioning.md.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace server {
namespace lwm2m {

/// One (PSK identity → hex key) pair to feed DTLSAdapter::add_credential.
struct CredPair {
    std::string identity;
    std::string key_hex;
};

/// Cloud canonical identity for a raw device serial: rpi<serial>@cloud.local.
std::string format_identity(const std::string& serial);

/// Mint a cryptographically-random `nbytes` (default 32) DM PSK, lowercase
/// hex-encoded (2*nbytes chars). Throws std::runtime_error if no entropy
/// source is available. The BS PSK is device-generated; this is the cloud's
/// per-endpoint DM PSK.
std::string generate_psk_hex(std::size_t nbytes = 32);

/// Insert or replace the credential record for `serial` in the JSON array
/// string `array_json`. The DM identity is the same formatted identity as
/// the BS. Idempotent on serial (an existing entry is replaced). Returns the
/// updated array as a compact JSON string. Throws std::runtime_error if
/// `array_json` is not a JSON array.
std::string upsert_credential(const std::string& array_json,
                              const std::string& serial,
                              const std::string& bs_psk_hex,
                              const std::string& dm_psk_hex);

/// Remove the record for `serial`. No-op if absent. Returns the updated array.
std::string remove_credential(const std::string& array_json,
                              const std::string& serial);

/// Merge a minted VPN client credential (PEM) into the record for `serial`,
/// adding/updating the "vpn.client.cert" + "vpn.client.key" fields. Creates a
/// minimal record (serial + identity) if none exists yet. Phase-3 delivery
/// reads these and pushes them to the device over LwM2M Object 2048; the
/// BS/DM PSK loaders ignore the extra fields. Returns the updated array.
/// Throws std::runtime_error if `array_json` is not a JSON array.
std::string upsert_vpn_cert(const std::string& array_json,
                            const std::string& serial,
                            const std::string& client_cert_pem,
                            const std::string& client_key_pem);

/// Task N — turn the credential array into the (identity → key) pairs a
/// server instance must register with its DTLS adapter:
///   * BS server (is_bs=true): identity = RAW serial (what the device puts
///     on the wire), key = bs.psk.key.
///   * DM server (is_bs=false): identity = dm.psk.id (rpi<serial>@cloud.local,
///     which the device sends after bootstrap), key = dm.psk.key.
/// Records missing the relevant key are skipped. Throws if not an array.
std::vector<CredPair> credentials_for_instance(const std::string& array_json,
                                               bool is_bs);

} // namespace lwm2m
} // namespace server

#endif /* __server_lwm2m_cloud_credentials_hpp__ */
