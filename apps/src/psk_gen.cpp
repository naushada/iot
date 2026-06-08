#include "psk_gen.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>

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

} // namespace iot
