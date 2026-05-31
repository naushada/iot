#ifndef __apps_ds_config_hpp__
#define __apps_ds_config_hpp__

/// Live config-plane reader backed by the data-store unix-socket
/// service.
///
/// The iot binary tries to connect to ds-server (default
/// `/var/run/iot/data_store.sock`, overridable via `ds-sock=PATH` on
/// the CLI). On success, the reader:
///
///   * primes its cache with a `get` for the three known iot.* keys,
///   * registers a `watch` so subsequent ds-cli mutations land in the
///     cache + fire `on_change()` callbacks,
///   * keeps the underlying `data_store::Client` alive for the life
///     of this object (do NOT destroy DsConfig until shutdown).
///
/// When the connection fails (server down, socket missing, perms
/// denied) every accessor returns nullopt and the caller falls back
/// to its compiled-in / lua_config defaults. ds-server is strictly
/// opt-in — its absence MUST NOT break startup.
///
/// Accessor pattern:
///
///   const std::string endpoint =
///       ds.endpoint().value_or(cli_arg_endpoint);
///   const std::uint32_t lifetime =
///       ds.lifetime().value_or(86400);
///
/// Hot-reload note: today this class only LOGS observed changes (via
/// ACE_DEBUG/LM_INFO). Re-applying mutated values to the live LwM2M
/// FSM (re-register on endpoint change, re-bind on server.uri change,
/// etc.) is FUP-DS-9 — see modules/data-store/docs/client_api.md §11
/// for the recommended apply policies.
///
/// Threading: all accessors are thread-safe (internal mutex). The
/// data_store::Client's listener thread is the source of cache
/// updates; main-thread accessors take the same mutex for read.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace iot {

class DsConfig {
public:
    /// One of these is passed to a `ChangeCallback` to say what was
    /// observed. Cache is already updated when the callback fires.
    enum class Key {
        Endpoint,
        ServerUri,
        Lifetime,
    };

    using ChangeCallback = std::function<void(Key)>;

    /// Connect to `socketPath` (or kDefault when empty), prime the
    /// cache, register the watch. Connection failures are silently
    /// absorbed — call `connected()` to check.
    explicit DsConfig(std::string socketPath = {});
    ~DsConfig();

    DsConfig(const DsConfig&)            = delete;
    DsConfig& operator=(const DsConfig&) = delete;

    bool connected() const { return m_ok; }
    const std::string& socket_path() const { return m_path; }

    /// Snapshot accessors — return whatever was last observed in the
    /// store (initial `get` + every subsequent `NotifyEvent` push).
    /// Thread-safe.
    std::optional<std::string>   endpoint()   const;
    std::optional<std::string>   server_uri() const;
    std::optional<std::uint32_t> lifetime()   const;

    /// Register a per-key change listener. The callback fires on the
    /// data_store::Client's listener thread — keep it short, don't
    /// block on locks the main thread might hold.
    void on_change(ChangeCallback cb);

    /// Default socket path when none is supplied. Tracks ds-server's
    /// kDefaultSocketPath, duplicated here so the public header stays
    /// free of data_store/ includes.
    static const char* kDefaultSocketPath;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string           m_path;
    bool                  m_ok = false;
};

} // namespace iot

#endif /* __apps_ds_config_hpp__ */
