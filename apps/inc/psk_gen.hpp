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

} // namespace iot

#endif /* __iot_psk_gen_hpp__ */
