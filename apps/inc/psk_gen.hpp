#ifndef __iot_psk_gen_hpp__
#define __iot_psk_gen_hpp__

/// PSK provisioning (task B) — 32-byte PSK generator (C++).
///
/// The device's BS PSK is generated in the browser by device-ui; this
/// C++ generator is used cloud-side to mint the per-endpoint DM PSK at
/// provisioning time. Output is hex so it drops straight into the
/// data-store opaque PSK keys and the tinydtls hex→binary path.
///
/// See apps/docs/tdd-psk-provisioning.md.

#include <cstddef>
#include <optional>
#include <string>

namespace iot {

/// Generate `nbytes` (default 32) of CSPRNG output, hex-encoded as
/// lowercase. Returns a 2*nbytes character string. Throws
/// std::runtime_error if no entropy source is available.
std::string generate_psk_hex(std::size_t nbytes = 32);

/// Lowercase hex-encode `len` bytes at `data`.
std::string hex_encode(const unsigned char* data, std::size_t len);

/// Decode a hex string to raw bytes. Returns "" on odd length or any
/// non-hex character (case-insensitive input accepted).
std::string hex_decode(const std::string& hex);

/// SHA-256 of `input`, lowercase hex (64 chars). Used to derive the BS DTLS
/// PSK identity from the endpoint: identity = sha256_hex(endpoint). Both the
/// device client and the cloud BS compute this identically, so the identity
/// never has to be stored/commissioned — only the endpoint + secret.
std::string sha256_hex(const std::string& input);

/// HKDF-SHA256 (RFC 5869, extract-then-expand) over raw bytes, returning the
/// `out_len`-byte output key material as lowercase hex (2*out_len chars).
/// A zero-length `salt` is substituted with HashLen (32) zero bytes per
/// RFC 5869 §2.2 so callers and the host-side Python generator agree exactly.
/// Throws std::runtime_error on any OpenSSL failure.
std::string hkdf_sha256(const std::string& ikm,
                        const std::string& salt,
                        const std::string& info,
                        std::size_t        out_len = 32);

/// Derive a device's per-unit Bootstrap PSK from the cloud master + the device
/// serial (zero-touch HKDF tier — see apps/docs/tdd-bs-hkdf-zerotouch.md).
///
///   psk = HKDF-SHA256(ikm = hex_decode(master_hex),
///                     salt = "" (-> 32 zero bytes),
///                     info = "iot-bs-psk:v1:" + serial,
///                     L    = 32)
///
/// Returns the 64-char lowercase-hex PSK. Both the cloud BS server (to
/// authenticate the handshake) and the host flashing tool (gen_bs_psk.py, to
/// inject the key) call this and MUST agree byte-for-byte. The `v1` tag in
/// `info` versions the master for rotation. Returns "" when `master_hex` is
/// empty or not valid hex — the caller treats that as "HKDF tier disabled".
std::string derive_bs_psk_hex(const std::string& master_hex,
                              const std::string& serial);

/// Derive a device's per-unit Device-Management PSK from the same cloud master
/// + serial, with a DM-specific `info` tag so the DM key is independent of the
/// BS key:
///   psk = HKDF-SHA256(hex_decode(master_hex), "", "iot-dm-psk:v1:"+serial, 32)
/// Lets the zero-touch path stay fully stateless — the BS pushes derived DM
/// creds during /bs and the DM server re-derives the same key from the
/// presented identity, so nothing is minted or stored. Returns "" on an empty
/// or invalid master.
std::string derive_dm_psk_hex(const std::string& master_hex,
                              const std::string& serial);

/// Standard base64 (RFC 4648, '+/' alphabet, '=' padding).
std::string base64_encode(const std::string& bin);
/// Decode standard base64. Returns "" on any invalid character or length.
std::string base64_decode(const std::string& b64);

// ── BS master key at rest: AES-256-GCM envelope (KMS-wrapped) ────────────────
//
// The HKDF master is never stored in the clear. It lives in the data-store
// (`cloud.bs.master.key`) as `base64( nonce(12) || ciphertext || tag(16) )`,
// AES-256-GCM under a Key-Encryption-Key (KEK) that is delivered to the cloud
// BS process out-of-band (systemd LoadCredential / env), never via the
// data-store. The process unwraps once at startup and holds the master in
// memory only. AAD binds the blob to its purpose. See
// apps/docs/tdd-bs-hkdf-zerotouch.md.

/// AAD tag tying a wrapped blob to its purpose+version (rotate with the key).
constexpr const char* kBsMasterAad = "iot-bs-master:v1";

/// Wrap a hex master under a hex KEK with the given 12-byte (24-hex) nonce,
/// returning the base64 envelope. Production callers (the bs-master-wrap tool)
/// pass a fresh random nonce; tests pin it for a deterministic vector. Returns
/// "" if the KEK is not 32 bytes, the nonce not 12 bytes, or the master not
/// valid hex.
std::string wrap_bs_master(const std::string& kek_hex,
                           const std::string& master_hex,
                           const std::string& nonce_hex);

/// Unwrap a base64 envelope under a hex KEK, returning the master as hex.
/// Returns std::nullopt on any failure — empty inputs, bad base64, wrong KEK,
/// or a failed GCM tag (tamper) — which the caller treats as "HKDF tier
/// unavailable / disabled" (fail closed; never derive against a bad master).
std::optional<std::string> unwrap_bs_master_hex(const std::string& kek_hex,
                                                const std::string& wrapped_b64);

} // namespace iot

#endif /* __iot_psk_gen_hpp__ */
