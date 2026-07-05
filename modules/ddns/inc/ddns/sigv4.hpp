#ifndef __ddns_sigv4_hpp__
#define __ddns_sigv4_hpp__

#include <string>
#include <vector>

/**
 * @file sigv4.hpp
 * @brief Minimal AWS Signature Version 4 signer (for the Route53 backend).
 *
 * Implements the SigV4 canonical-request → string-to-sign → signing-key →
 * signature chain over OpenSSL HMAC-SHA256. Only what Route53
 * ChangeResourceRecordSets needs: a signed POST with host + x-amz-date headers.
 * See https://docs.aws.amazon.com/general/latest/gr/sigv4_signing.html
 */

namespace ddns {

/// hex(sha256(data)).
std::string sha256_hex(const std::string& data);

/// The 32-byte SigV4 signing key. Exposed for unit testing against the AWS
/// documented derivation vector.
///   kDate    = HMAC("AWS4"+secret, datestamp)   datestamp = "YYYYMMDD"
///   kRegion  = HMAC(kDate,   region)
///   kService = HMAC(kRegion, service)
///   kSigning = HMAC(kService,"aws4_request")
std::vector<unsigned char> sigv4_signing_key(const std::string& secret,
                                             const std::string& datestamp,
                                             const std::string& region,
                                             const std::string& service);

/// Compute the request headers (Host, x-amz-date, Authorization) for a SigV4
/// request. `amz_date` is "YYYYMMDDTHHMMSSZ" (caller supplies now, or a fixed
/// value in tests). Signs the `host;x-amz-date` header set. Returns header
/// lines ready to hand to http_request().
std::vector<std::string> sigv4_headers(const std::string& method,
                                       const std::string& host,
                                       const std::string& canonical_uri,
                                       const std::string& canonical_query,
                                       const std::string& payload,
                                       const std::string& region,
                                       const std::string& service,
                                       const std::string& access_key,
                                       const std::string& secret_key,
                                       const std::string& amz_date);

/// Current UTC time as "YYYYMMDDTHHMMSSZ" (via ACE_OS, no raw <ctime>).
std::string sigv4_now_amz_date();

} // namespace ddns

#endif /* __ddns_sigv4_hpp__ */
