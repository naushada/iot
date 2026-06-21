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

namespace data_store { class Client; }   // forward decl

namespace iot {

class DsConfig {
public:
    /// One of these is passed to a `ChangeCallback` to say what was
    /// observed. Cache is already updated when the callback fires.
    enum class Key {
        Endpoint,
        ServerUri,
        BsUri,
        Lifetime,
        // PSK provisioning (tasks E/F/G).
        Serial,
        BsPskIdentity,
        BsPskKey,
        BsPskOverride,
        DmPskIdentity,
        DmPskKey,
        DevMode,
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
    std::optional<std::string>   bs_uri()     const;  // iot.bs.uri (commissioned)
    std::optional<std::uint32_t> lifetime()   const;

    // PSK provisioning accessors (task E/F/G). All return nullopt when
    // !connected() or the key is unset. The client runs as `engineer`
    // so the read_acl on the PSK keys is satisfied.
    std::optional<std::string>   serial()           const;
    std::optional<std::string>   bs_psk_identity()  const;
    std::optional<std::string>   bs_psk_key()        const;
    /// True when the operator pinned a custom BS PSK identity/key for a
    /// third-party bootstrap server. When set, the client uses
    /// bs_psk_identity() verbatim instead of deriving sha256(endpoint).
    bool                         bs_psk_override()  const;
    std::optional<std::string>   dm_psk_identity()  const;
    std::optional<std::string>   dm_psk_key()        const;
    bool                         dev_mode()         const;

    // Writers (task E/F). Persist to the data-store; return false when
    // !connected() or the set is rejected (e.g. ACL without dev-mode).
    /// RPi auto-fill: persist serial → iot.serial + iot.endpoint +
    /// iot.bs.psk.identity (all = raw serial).
    bool set_serial(const std::string& serial);
    /// Bootstrap delivered DM credentials → iot.dm.psk.identity / .key.
    bool set_dm_credentials(const std::string& identity,
                            const std::string& key_hex);
    /// Persist the bootstrap-delivered DM Server URI (Security Object RID 0
    /// of the non-bootstrap PSK account) → iot.dm.uri, so the device-ui can
    /// display the URI the device actually registered to. Write-only here —
    /// the client is the sole writer, so there's nothing to mirror locally.
    bool set_dm_uri(const std::string& uri);
    /// Derive the VPN server host from the bootstrap-delivered DM URI and
    /// persist it → vpn.remote.host, so a co-located cloud (VPN concentrator
    /// on the same VM as the DM) needs no separate VPN-host config on the
    /// device. The cloud's Object-2048 endpoint push still overrides it for a
    /// split topology. No-op when !connected() or host is empty.
    bool set_vpn_remote_host(const std::string& host);
    /// Publish the LwM2M connection lifecycle token to iot.conn.state so
    /// the device-ui can render real-time progress. One of: idle /
    /// bootstrapping / bootstrapped / dm-connecting / dm-connected /
    /// registered / failed. No-op when !connected().
    bool set_conn_state(const std::string& state);

    /// Register a per-key change listener. The callback fires on the
    /// data_store::Client's listener thread — keep it short, don't
    /// block on locks the main thread might hold.
    void on_change(ChangeCallback cb);

    /// Access the underlying data_store::Client so the L16/D5
    /// ServiceGate can share one socket + listener thread between
    /// the iot.* watch and the services.lwm2m.{client,server}.enable
    /// watch. nullptr when !connected().
    data_store::Client* client();

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
