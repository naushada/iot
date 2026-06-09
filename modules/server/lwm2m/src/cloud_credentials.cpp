#include "cloud_credentials.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace server {
namespace lwm2m {

using nlohmann::json;

namespace {
// SHA-256 → lowercase hex. The BS DTLS PSK identity is derived as
// sha256(endpoint) on BOTH the device and the cloud, so it is never stored.
// (endpoint == serial in this model.) Must match apps iot::sha256_hex.
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
    static const char* k = "0123456789abcdef";
    std::string out; out.reserve(dlen * 2);
    for (unsigned int i = 0; i < dlen; ++i) {
        out.push_back(k[(digest[i] >> 4) & 0xF]);
        out.push_back(k[digest[i] & 0xF]);
    }
    return out;
}
} // namespace

std::string format_identity(const std::string& serial) {
    return "rpi" + serial + "@cloud.local";
}

std::string generate_psk_hex(std::size_t nbytes) {
    std::vector<unsigned char> buf(nbytes);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.is_open())
        throw std::runtime_error("generate_psk_hex: no entropy source");
    urandom.read(reinterpret_cast<char*>(buf.data()),
                 static_cast<std::streamsize>(nbytes));
    if (static_cast<std::size_t>(urandom.gcount()) != nbytes)
        throw std::runtime_error("generate_psk_hex: short read from urandom");
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(nbytes * 2);
    for (unsigned char b : buf) {
        out.push_back(k[(b >> 4) & 0xF]);
        out.push_back(k[b & 0xF]);
    }
    return out;
}

namespace {
json parse_array(const std::string& array_json) {
    json arr = json::parse(array_json.empty() ? "[]" : array_json);
    if (!arr.is_array())
        throw std::runtime_error("cloud.endpoint.credentials is not an array");
    return arr;
}
} // namespace

std::string upsert_credential(const std::string& array_json,
                              const std::string& serial,
                              const std::string& bs_psk_hex,
                              const std::string& dm_psk_hex) {
    json arr = parse_array(array_json);
    const std::string identity = format_identity(serial);

    // JSON field names follow the dotted convention used by
    // cloud.provision.configs (sec.uri, dm.psk.id, …).
    json rec = {
        {"serial",     serial},
        {"identity",   identity},
        {"bs.psk.key", bs_psk_hex},
        {"dm.psk.id",  identity},
        {"dm.psk.key", dm_psk_hex},
    };

    // Replace an existing entry for this serial (idempotent provision).
    for (auto& e : arr) {
        if (e.is_object() && e.value("serial", "") == serial) {
            e = rec;
            return arr.dump();
        }
    }
    arr.push_back(rec);
    return arr.dump();
}

std::string upsert_vpn_cert(const std::string& array_json,
                            const std::string& serial,
                            const std::string& client_cert_pem,
                            const std::string& client_key_pem) {
    json arr = parse_array(array_json);
    for (auto& e : arr) {
        if (e.is_object() && e.value("serial", "") == serial) {
            e["vpn.client.cert"] = client_cert_pem;
            e["vpn.client.key"]  = client_key_pem;
            return arr.dump();
        }
    }
    // No PSK record yet (cert minted before/without PSK provisioning) — create
    // a minimal record so the cert isn't lost.
    json rec = {
        {"serial",          serial},
        {"identity",        format_identity(serial)},
        {"vpn.client.cert", client_cert_pem},
        {"vpn.client.key",  client_key_pem},
    };
    arr.push_back(rec);
    return arr.dump();
}

std::string remove_credential(const std::string& array_json,
                              const std::string& serial) {
    json arr = parse_array(array_json);
    json out = json::array();
    for (auto& e : arr) {
        if (e.is_object() && e.value("serial", "") == serial) continue;
        out.push_back(e);
    }
    return out.dump();
}

std::vector<CredPair> credentials_for_instance(const std::string& array_json,
                                               bool is_bs) {
    json arr = parse_array(array_json);
    std::vector<CredPair> out;
    for (auto& e : arr) {
        if (!e.is_object()) continue;
        if (is_bs) {
            const std::string serial = e.value("serial", "");
            const std::string key    = e.value("bs.psk.key", "");
            // BS DTLS identity = sha256(endpoint); endpoint == serial here.
            // The device derives the same, so it is never sent in the clear
            // as the readable serial.
            // 128-bit identity: first 32 hex chars of sha256(serial),
            // matching the device's iot::sha256_hex(endpoint).substr(0,32).
            if (!serial.empty() && !key.empty())
                out.push_back({sha256_hex(serial).substr(0, 32), key});
        } else {
            const std::string id  = e.value("dm.psk.id", "");
            const std::string key = e.value("dm.psk.key", "");
            if (!id.empty() && !key.empty())
                out.push_back({id, key});
        }
    }
    return out;
}

} // namespace lwm2m
} // namespace server
