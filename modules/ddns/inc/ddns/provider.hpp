#ifndef __ddns_provider_hpp__
#define __ddns_provider_hpp__

#include <memory>
#include <string>

/**
 * @file provider.hpp
 * @brief ProviderBackend strategy interface for the DDNS daemon.
 *
 * Each supported provider (dyndns2, duckdns, cloudflare, route53) implements
 * this interface; the daemon core stays provider-agnostic and just calls
 * update() with the detected public IP. Concrete impls live in
 * modules/ddns/src/provider_<name>.cpp and are wired in from later PRs
 * (FR-4..FR-7 / #516..#519). PR-1 ships only this contract.
 */

namespace ddns {

/// Provider credentials + targets, mapped per-backend. The daemon fills these
/// from the ddns.* ds keys (secrets from the write-only keys or the credential
/// file). `secret` is never logged.
struct Creds {
    std::string user;       ///< dyndns2 user / cf zone id / r53 access key
    std::string secret;     ///< dyndns2 token / duckdns token / cf token / r53 secret
    std::string target1;    ///< dyndns2 server / duckdns domains / cf record name / r53 zone id
    std::string target2;    ///< cf zone id / r53 record name (as needed)
};

/// Outcome of an update() call. `msg` must be sanitized — no secrets.
struct Result {
    bool        ok   = false;
    int         code = 0;       ///< HTTP status or provider-specific code
    std::string msg;            ///< human-readable, no secrets
};

/// A single DNS provider backend.
struct ProviderBackend {
    virtual ~ProviderBackend() = default;

    /// Point `host`'s A record at `ip` using `creds`. Idempotent on the
    /// provider side (a re-push of the same IP is a success/no-op).
    virtual Result update(const std::string& host,
                          const std::string& ip,
                          const Creds&       creds) = 0;

    /// Backend name, for logging/state ("dyndns2", "duckdns", ...).
    virtual const char* name() const = 0;
};

/// Factory: build the backend for a ddns.provider value, or nullptr if unknown.
/// Defined once the concrete backends land (PR-3+).
std::unique_ptr<ProviderBackend> make_backend(const std::string& provider);

} // namespace ddns

#endif /* __ddns_provider_hpp__ */
