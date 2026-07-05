// Unit tests for the DDNS core: IP validation, URL encoding, provider IP-source
// parsing, SHA-256, and the AWS SigV4 signing-key derivation vector. These
// exercise pure logic only — no network / curl.

#include <gtest/gtest.h>

#include "ddns/http_client.hpp"
#include "ddns/public_ip.hpp"
#include "ddns/sigv4.hpp"

using namespace ddns;

// ── validate_ipv4 ───────────────────────────────────────────────────────────
TEST(ValidateIpv4, AcceptsWellFormed) {
    EXPECT_EQ(validate_ipv4("1.2.3.4"), "1.2.3.4");
    EXPECT_EQ(validate_ipv4("255.255.255.255"), "255.255.255.255");
    EXPECT_EQ(validate_ipv4("0.0.0.0"), "0.0.0.0");
}
TEST(ValidateIpv4, TrimsWhitespace) {
    EXPECT_EQ(validate_ipv4("  8.8.8.8\n"), "8.8.8.8");
    EXPECT_EQ(validate_ipv4("203.0.113.5\r\n"), "203.0.113.5");
}
TEST(ValidateIpv4, RejectsMalformed) {
    EXPECT_EQ(validate_ipv4("256.0.0.1"), "");     // octet > 255
    EXPECT_EQ(validate_ipv4("1.2.3"), "");         // too few octets
    EXPECT_EQ(validate_ipv4("1.2.3.4.5"), "");     // too many
    EXPECT_EQ(validate_ipv4("1.2.3."), "");        // trailing dot
    EXPECT_EQ(validate_ipv4("a.b.c.d"), "");       // non-numeric
    EXPECT_EQ(validate_ipv4(""), "");
    EXPECT_EQ(validate_ipv4("::1"), "");           // IPv6 rejected (v1 is A only)
}

// ── url_encode ──────────────────────────────────────────────────────────────
TEST(UrlEncode, PassesUnreserved) {
    EXPECT_EQ(url_encode("abcXYZ-_.~09"), "abcXYZ-_.~09");
}
TEST(UrlEncode, EncodesReserved) {
    EXPECT_EQ(url_encode("a b"), "a%20b");
    EXPECT_EQ(url_encode("x@y.com"), "x%40y.com");
    EXPECT_EQ(url_encode("a/b?c=d&e"), "a%2Fb%3Fc%3Dd%26e");
}

// ── parse_ip_source ─────────────────────────────────────────────────────────
TEST(IpSource, Parse) {
    EXPECT_EQ(parse_ip_source("echo"), IpSource::Echo);
    EXPECT_EQ(parse_ip_source("dyndns2-auto"), IpSource::Dyndns2Auto);
    EXPECT_EQ(parse_ip_source("cloud"), IpSource::Cloud);
    EXPECT_EQ(parse_ip_source("garbage"), IpSource::Echo);  // default
}

// ── sha256_hex ──────────────────────────────────────────────────────────────
TEST(Sha256, KnownVectors) {
    EXPECT_EQ(sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ── SigV4 signing key (AWS documented derivation example) ───────────────────
// https://docs.aws.amazon.com/general/latest/gr/sigv4-calculate-signature.html
//   secret=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY dateStamp=20120215
//   region=us-east-1 service=iam  → known 32-byte signing key.
static std::string hex(const std::vector<unsigned char>& v) {
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : v) { out.push_back(h[c >> 4]); out.push_back(h[c & 0xF]); }
    return out;
}
TEST(SigV4, SigningKeyVector) {
    auto key = sigv4_signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
                                 "20120215", "us-east-1", "iam");
    EXPECT_EQ(hex(key),
              "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");
}
TEST(SigV4, HeadersWellFormed) {
    auto h = sigv4_headers("POST", "route53.amazonaws.com",
                           "/2013-04-01/hostedzone/Z1/rrset", "", "<x/>",
                           "us-east-1", "route53", "AKID", "secret",
                           "20240101T000000Z");
    ASSERT_EQ(h.size(), 3u);
    EXPECT_EQ(h[0], "Host: route53.amazonaws.com");
    EXPECT_EQ(h[1], "x-amz-date: 20240101T000000Z");
    EXPECT_NE(h[2].find("AWS4-HMAC-SHA256 Credential=AKID/20240101/us-east-1/route53/aws4_request"),
              std::string::npos);
    EXPECT_NE(h[2].find("SignedHeaders=host;x-amz-date"), std::string::npos);
}
