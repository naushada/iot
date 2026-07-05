#ifndef __ddns_http_client_hpp__
#define __ddns_http_client_hpp__

#include <string>
#include <vector>

/**
 * @file http_client.hpp
 * @brief Minimal outbound HTTP(S) transport for the DDNS daemon.
 *
 * Shells out to the `curl` CLI via ACE_Process — matching the OTA download +
 * container-registry-puller idiom (modules/containers/daemon/http_client.cpp).
 * No libcurl link dependency; curl is a runtime RDEPENDS. Every option goes
 * through a temp `-K` config file (ACE's argv round-trip shatters multi-word
 * header values), and the request/response bodies go through temp files.
 *
 * BLOCKING — the caller (the daemon tick) blocks until curl exits, like
 * cellular-client's synchronous modem poll. Fine for the infrequent, short DDNS
 * requests; do not call from a latency-sensitive path.
 */

namespace ddns {

struct HttpResponse {
    bool        transport_ok = false;  ///< curl produced an HTTP response
    long        status       = 0;      ///< final HTTP status across redirects
    std::string headers;               ///< raw response headers (all hops)
    std::string body;                  ///< response body (small; read into mem)
};

/// Generic HTTP(S) request.
///   method  - "GET" | "PUT" | "POST" | "DELETE"
///   headers - full "Key: Value" lines (Authorization, Content-Type, x-amz-*)
///   basic   - "user:password" for HTTP Basic ("" = none)
///   body    - request body ("" = none; sent verbatim via --data-binary)
/// Returns true when curl produced an HTTP response (even 4xx/5xx — inspect
/// resp.status); false only on transport failure (curl missing/DNS/TLS), with
/// `err` set. Never logs `basic`/`headers` (may carry secrets).
bool http_request(const std::string&              method,
                  const std::string&              url,
                  const std::vector<std::string>& headers,
                  const std::string&              basic,
                  const std::string&              body,
                  int                             timeout_sec,
                  HttpResponse&                   resp,
                  std::string&                    err);

/// Convenience GET with no auth/headers.
bool http_get(const std::string& url, int timeout_sec,
              HttpResponse& resp, std::string& err);

/// Percent-encode a string for use in a URL query component (RFC 3986
/// unreserved chars pass through; everything else → %XX).
std::string url_encode(const std::string& s);

} // namespace ddns

#endif /* __ddns_http_client_hpp__ */
