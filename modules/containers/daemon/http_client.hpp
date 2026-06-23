#ifndef __iot_container_http_client_hpp__
#define __iot_container_http_client_hpp__

#include <string>
#include <vector>

/// Thin HTTP(S) transport for the registry puller. Shells out to the `curl`
/// CLI via ACE_Process (matching the OTA download path — no libcurl link dep),
/// plus an OpenSSL streaming SHA-256 for blob verification. Kept out of the
/// pure containers_core so that core stays network/ACE-free + host-testable.

namespace containers {

struct HttpResponse {
    bool        transport_ok = false;  ///< curl produced an HTTP response
    long        status = 0;            ///< final HTTP status code
    std::string headers;               ///< raw response headers (curl -D, all hops)
};

/// HTTP GET via curl. Body is written to `body_path` ("" → discard). Optional
/// auth: `bearer` → `Authorization: Bearer …`; `basic` ("user:pass") → HTTP
/// Basic (`curl -u`). Each `accept` entry becomes an `Accept:` header.
/// `follow_redirects` adds `-L` (blob GETs 307 to a CDN).
///
/// Returns true when curl produced an HTTP response — even 4xx/5xx, so the
/// caller inspects `resp.status` (401 → auth, 404 → not found). Returns false
/// only on transport failure (curl missing, DNS, connect, TLS); `err` is set.
bool http_get(const std::string&              url,
              const std::vector<std::string>& accept,
              const std::string&              bearer,
              const std::string&              basic,
              const std::string&              body_path,
              bool                            follow_redirects,
              int                             timeout_sec,
              HttpResponse&                   resp,
              std::string&                    err);

/// Streaming SHA-256 of a file → lowercase hex. False on open/read error.
bool sha256_file(const std::string& path, std::string& hex_out);

/// `mkdir -p` for an absolute path (each component, mode 0700, EEXIST ok).
bool mkdir_p(const std::string& path);

} // namespace containers

#endif /* __iot_container_http_client_hpp__ */
