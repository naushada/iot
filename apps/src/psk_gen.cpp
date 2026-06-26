#include "psk_gen.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>
#include <openssl/kdf.h>

namespace iot {

namespace {

bool fill_random(unsigned char* buf, std::size_t len) {
    // Portable across the Linux targets we care about: read from
    // /dev/urandom. (getrandom(2) would also work but pulls in a
    // platform header; the file path keeps this trivially testable.)
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.is_open()) return false;
    urandom.read(reinterpret_cast<char*>(buf),
                 static_cast<std::streamsize>(len));
    return static_cast<std::size_t>(urandom.gcount()) == len;
}

} // namespace

std::string hex_encode(const unsigned char* data, std::size_t len) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(k[(data[i] >> 4) & 0xF]);
        out.push_back(k[data[i] & 0xF]);
    }
    return out;
}

std::string hex_decode(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string generate_psk_hex(std::size_t nbytes) {
    std::vector<unsigned char> buf(nbytes);
    if (!fill_random(buf.data(), nbytes))
        throw std::runtime_error("generate_psk_hex: no entropy source");
    return hex_encode(buf.data(), nbytes);
}

std::string sha256_hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("sha256_hex: EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &dlen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("sha256_hex: digest failed");
    }
    EVP_MD_CTX_free(ctx);
    return hex_encode(digest, dlen);
}

std::string hkdf_sha256(const std::string& ikm,
                        const std::string& salt,
                        const std::string& info,
                        std::size_t        out_len) {
    // RFC 5869 §2.2: an empty salt is substituted with HashLen zero bytes.
    // Doing this explicitly (rather than relying on OpenSSL's no-salt default)
    // keeps the result identical to the host-side Python generator, which does
    // the same substitution.
    static const unsigned char zeros[32] = {0};
    const unsigned char* salt_ptr =
        salt.empty() ? zeros
                     : reinterpret_cast<const unsigned char*>(salt.data());
    const int salt_len = salt.empty() ? static_cast<int>(sizeof zeros)
                                       : static_cast<int>(salt.size());

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) throw std::runtime_error("hkdf_sha256: ctx alloc failed");

    std::vector<unsigned char> out(out_len);
    std::size_t outlen = out_len;
    const bool ok =
        EVP_PKEY_derive_init(pctx) == 1 &&
        EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt_ptr, salt_len) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key(
            pctx, reinterpret_cast<const unsigned char*>(ikm.data()),
            static_cast<int>(ikm.size())) == 1 &&
        EVP_PKEY_CTX_add1_hkdf_info(
            pctx, reinterpret_cast<const unsigned char*>(info.data()),
            static_cast<int>(info.size())) == 1 &&
        EVP_PKEY_derive(pctx, out.data(), &outlen) == 1 &&
        outlen == out_len;
    EVP_PKEY_CTX_free(pctx);
    if (!ok) throw std::runtime_error("hkdf_sha256: derive failed");
    return hex_encode(out.data(), out_len);
}

std::string derive_bs_psk_hex(const std::string& master_hex,
                              const std::string& serial) {
    const std::string ikm = hex_decode(master_hex);
    if (ikm.empty()) return {};   // empty or malformed master ⇒ tier disabled
    return hkdf_sha256(ikm, /*salt=*/"", "iot-bs-psk:v1:" + serial, 32);
}

} // namespace iot
