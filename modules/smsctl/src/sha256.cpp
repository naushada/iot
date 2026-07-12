#include "smsctl/session.hpp"

#include <openssl/sha.h>

namespace smsctl {

/// Byte-for-byte the same construction as iot-httpd's CredentialStore
/// (modules/http-server/src/auth.cpp) — SMS login MUST accept exactly the
/// passwords the device-ui login accepts, hashes live in the same ds keys.
std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

} // namespace smsctl
