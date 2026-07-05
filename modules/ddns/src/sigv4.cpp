#include "ddns/sigv4.hpp"

#include <ctime>

#include <ace/OS_NS_time.h>
#include <ace/OS_NS_sys_time.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace ddns {

namespace {

std::string to_hex(const unsigned char* p, unsigned len) {
    static const char* h = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned i = 0; i < len; ++i) {
        out.push_back(h[p[i] >> 4]);
        out.push_back(h[p[i] & 0x0F]);
    }
    return out;
}

std::vector<unsigned char> hmac_sha256(const unsigned char* key, int key_len,
                                       const std::string& data) {
    unsigned int len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), key, key_len,
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(), out, &len);
    return std::vector<unsigned char>(out, out + len);
}

std::vector<unsigned char> hmac_sha256(const std::vector<unsigned char>& key,
                                       const std::string& data) {
    return hmac_sha256(key.data(), static_cast<int>(key.size()), data);
}

} // namespace

std::string sha256_hex(const std::string& data) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
    return to_hex(md, SHA256_DIGEST_LENGTH);
}

std::vector<unsigned char> sigv4_signing_key(const std::string& secret,
                                             const std::string& datestamp,
                                             const std::string& region,
                                             const std::string& service) {
    const std::string k0 = "AWS4" + secret;
    auto kDate    = hmac_sha256(reinterpret_cast<const unsigned char*>(k0.data()),
                                static_cast<int>(k0.size()), datestamp);
    auto kRegion  = hmac_sha256(kDate, region);
    auto kService = hmac_sha256(kRegion, service);
    return hmac_sha256(kService, "aws4_request");
}

std::vector<std::string> sigv4_headers(const std::string& method,
                                       const std::string& host,
                                       const std::string& canonical_uri,
                                       const std::string& canonical_query,
                                       const std::string& payload,
                                       const std::string& region,
                                       const std::string& service,
                                       const std::string& access_key,
                                       const std::string& secret_key,
                                       const std::string& amz_date) {
    const std::string datestamp = amz_date.substr(0, 8);        // YYYYMMDD
    const std::string payload_hash = sha256_hex(payload);
    const std::string signed_headers = "host;x-amz-date";

    // Canonical request.
    std::string canonical_headers =
        "host:" + host + "\n" + "x-amz-date:" + amz_date + "\n";
    std::string canonical_request =
        method + "\n" + canonical_uri + "\n" + canonical_query + "\n" +
        canonical_headers + "\n" + signed_headers + "\n" + payload_hash;

    // String to sign.
    const std::string scope =
        datestamp + "/" + region + "/" + service + "/aws4_request";
    std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
        sha256_hex(canonical_request);

    // Signature.
    auto signing_key = sigv4_signing_key(secret_key, datestamp, region, service);
    auto sig = hmac_sha256(signing_key, string_to_sign);
    const std::string signature = to_hex(sig.data(),
                                         static_cast<unsigned>(sig.size()));

    const std::string authorization =
        "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
        ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

    return {
        "Host: " + host,
        "x-amz-date: " + amz_date,
        "Authorization: " + authorization,
    };
}

std::string sigv4_now_amz_date() {
    ACE_Time_Value now = ACE_OS::gettimeofday();
    time_t t = now.sec();
    struct tm g;
    ACE_OS::gmtime_r(&t, &g);
    char buf[20];
    // YYYYMMDDTHHMMSSZ
    std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour, g.tm_min, g.tm_sec);
    return std::string(buf);
}

} // namespace ddns
