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

namespace {

const char kB64Alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64_encode(const std::string& bin) {
    std::string out;
    out.reserve((bin.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(bin.data());
    for (; i + 3 <= bin.size(); i += 3) {
        const unsigned n = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        out.push_back(kB64Alpha[(n >> 18) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 12) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 6) & 0x3F]);
        out.push_back(kB64Alpha[n & 0x3F]);
    }
    if (i + 1 == bin.size()) {
        const unsigned n = p[i] << 16;
        out.push_back(kB64Alpha[(n >> 18) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 12) & 0x3F]);
        out.append("==");
    } else if (i + 2 == bin.size()) {
        const unsigned n = (p[i] << 16) | (p[i + 1] << 8);
        out.push_back(kB64Alpha[(n >> 18) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 12) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(const std::string& b64) {
    if (b64.size() % 4 != 0) return {};
    std::string out;
    out.reserve(b64.size() / 4 * 3);
    for (std::size_t i = 0; i < b64.size(); i += 4) {
        const char c0 = b64[i], c1 = b64[i + 1], c2 = b64[i + 2], c3 = b64[i + 3];
        const int v0 = b64_val(c0), v1 = b64_val(c1);
        if (v0 < 0 || v1 < 0) return {};
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        if (c2 == '=') {
            if (c3 != '=' || i + 4 != b64.size()) return {};
            break;
        }
        const int v2 = b64_val(c2);
        if (v2 < 0) return {};
        out.push_back(static_cast<char>(((v1 & 0xF) << 4) | (v2 >> 2)));
        if (c3 == '=') {
            if (i + 4 != b64.size()) return {};
            break;
        }
        const int v3 = b64_val(c3);
        if (v3 < 0) return {};
        out.push_back(static_cast<char>(((v2 & 0x3) << 6) | v3));
    }
    return out;
}

std::string wrap_bs_master(const std::string& kek_hex,
                           const std::string& master_hex,
                           const std::string& nonce_hex) {
    const std::string kek   = hex_decode(kek_hex);
    const std::string ptext = hex_decode(master_hex);
    const std::string nonce = hex_decode(nonce_hex);
    if (kek.size() != 32 || nonce.size() != 12 || ptext.empty()) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("wrap_bs_master: ctx alloc failed");
    std::string ct(ptext.size(), '\0');
    unsigned char tag[16];
    int len = 0;
    int aad_len = 0;
    const bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(kek.data()),
            reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &aad_len,
            reinterpret_cast<const unsigned char*>(kBsMasterAad),
            static_cast<int>(std::char_traits<char>::length(kBsMasterAad))) == 1 &&
        EVP_EncryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(&ct[0]), &len,
            reinterpret_cast<const unsigned char*>(ptext.data()),
            static_cast<int>(ptext.size())) == 1 &&
        EVP_EncryptFinal_ex(ctx,
            reinterpret_cast<unsigned char*>(&ct[0]) + len, &len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("wrap_bs_master: encrypt failed");

    std::string blob = nonce + ct +
        std::string(reinterpret_cast<char*>(tag), sizeof tag);
    return base64_encode(blob);
}

std::optional<std::string> unwrap_bs_master_hex(const std::string& kek_hex,
                                                const std::string& wrapped_b64) {
    const std::string kek = hex_decode(kek_hex);
    if (kek.size() != 32) return std::nullopt;
    const std::string blob = base64_decode(wrapped_b64);
    if (blob.size() < 12 + 16 + 1) return std::nullopt;   // nonce+tag+≥1B ct

    const std::string nonce = blob.substr(0, 12);
    const std::string tag   = blob.substr(blob.size() - 16);
    const std::string ct    = blob.substr(12, blob.size() - 12 - 16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("unwrap_bs_master_hex: ctx alloc failed");
    std::string ptext(ct.size(), '\0');
    int len = 0, aad_len = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(kek.data()),
            reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &aad_len,
            reinterpret_cast<const unsigned char*>(kBsMasterAad),
            static_cast<int>(std::char_traits<char>::length(kBsMasterAad))) == 1 &&
        EVP_DecryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(&ptext[0]), &len,
            reinterpret_cast<const unsigned char*>(ct.data()),
            static_cast<int>(ct.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
            const_cast<char*>(tag.data())) == 1;
    // Final verifies the tag: >0 ⇒ authentic, ≤0 ⇒ tamper/wrong key.
    if (ok)
        ok = EVP_DecryptFinal_ex(
                 ctx, reinterpret_cast<unsigned char*>(&ptext[0]) + len, &len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::nullopt;
    return hex_encode(reinterpret_cast<const unsigned char*>(ptext.data()),
                      ptext.size());
}

} // namespace iot
