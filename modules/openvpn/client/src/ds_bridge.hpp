#ifndef __openvpn_client_ds_bridge_hpp__
#define __openvpn_client_ds_bridge_hpp__

/// Bridge between openvpn-client and ds-server.
///
/// Mirrors the pattern in apps/src/ds_config.cpp (iot.* keys) but
/// for the vpn.* namespace. Holds a data_store::Client for the
/// daemon lifetime; primes a thread-safe snapshot of the readable
/// keys on startup; registers a callback-style watch so subsequent
/// `ds-cli set vpn.*` mutations land in the cache and fire the
/// caller's on_change handler.
///
/// L12/D3 — first-cut hot-reload policy per R4 in log/L12/plan.md:
/// the watch logs "needs restart" on every change. Live re-application
/// (regenerating the openvpn .conf + restarting the subprocess) is
/// FUP-L12-1 once the lifecycle FSM (D6) is in place.
///
/// Threading: all read accessors and the missing_required() check
/// are thread-safe (internal mutex). The data_store::Client's
/// listener thread is the source of cache updates; main-thread
/// callers take the same mutex for read.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openvpn_client {

class DsBridge {
public:
    /// Which key did the on_change() callback fire for? Exposing the
    /// raw string would work too, but an enum makes switch-statements
    /// in the consumer exhaustive-warnings-friendly.
    enum class Key {
        RemoteHost,
        RemotePort,
        RemoteProto,
        CertPath,
        KeyPath,
        CaPath,
        Cipher,
        Dev,
        MgmtPort,
    };

    using ChangeCallback = std::function<void(Key)>;

    /// WAN gate callback. Fires when net.iface.active is set, cleared,
    /// or changes value. nullopt means the key is unset / empty
    /// ("WAN down"); a value means the highest-priority OPER UP iface
    /// net-router has currently selected. Fires on the data_store::
    /// Client listener thread — keep it short.
    using WanCallback = std::function<void(const std::optional<std::string>&)>;

    /// Connect to `socketPath` (empty → default
    /// /var/run/iot/data_store.sock), prime the snapshot, register the
    /// watch. Connection failures are silently absorbed — call
    /// connected() to check before exercising the accessors.
    explicit DsBridge(std::string socketPath = {});
    ~DsBridge();

    DsBridge(const DsBridge&)            = delete;
    DsBridge& operator=(const DsBridge&) = delete;

    bool connected() const { return m_ok; }
    const std::string& socket_path() const { return m_path; }

    // ─────────────────────── Read snapshots ────────────────────────
    // Return whatever the listener thread last observed. nullopt for
    // an unset key (where the schema has no default either).
    std::optional<std::string>   remote_host()  const;
    std::optional<std::uint32_t> remote_port()  const;
    std::optional<std::string>   remote_proto() const;
    std::optional<std::string>   cert_path()    const;
    std::optional<std::string>   key_path()     const;
    std::optional<std::string>   ca_path()      const;
    std::optional<std::string>   cipher()       const;
    std::optional<std::string>   dev()          const;
    std::optional<std::uint32_t> mgmt_port()    const;

    /// Snapshot of net.iface.active (the highest-priority OPER UP
    /// interface net-router has currently selected). nullopt while
    /// net-router has not yet written the key (e.g., during startup
    /// before any iface comes up) or when WAN is genuinely down (empty
    /// string written by net-router collapses to nullopt). Updated by
    /// the same listener thread that drives the vpn.* watch.
    std::optional<std::string>   wan_iface()    const;

    /// Returns nullopt when every required key is present
    /// (vpn.remote.host, vpn.cert.path, vpn.key.path, vpn.ca.path).
    /// Otherwise returns the list of missing key names so the caller
    /// can refuse to start with a clear diagnostic. Schema defaults
    /// satisfy the check for the optional keys (port/proto/cipher/dev/
    /// mgmt.port) — those never appear in this list.
    std::optional<std::vector<std::string>> missing_required() const;

    // ─────────────────────── Write side ────────────────────────────
    // Each setter issues a data_store::Client::set on the shared
    // socket. Best-effort: a wire-level failure is logged but not
    // surfaced — the caller's state machine has already advanced.

    void set_state(const std::string& s);
    void set_assigned_ip(const std::string& s);
    void set_assigned_gateway(const std::string& s);
    void set_assigned_netmask(const std::string& s);
    void set_assigned_dns(const std::string& s);
    void set_pid(std::uint32_t p);
    void set_exit_code(std::int32_t c);
    void set_gate_reason(const std::string& s);
    void set_bound_iface(const std::string& s);

    /// Register a per-key change listener. Fires on the
    /// data_store::Client's listener thread — keep it short, don't
    /// block on locks the main thread might hold.
    void on_change(ChangeCallback cb);

    /// Register a WAN gate listener. Fires when net.iface.active is
    /// set, cleared, or changes value. The callback receives the new
    /// snapshot (nullopt = WAN down). Same threading rules as
    /// on_change(): runs on the listener thread, do not block on the
    /// daemon's main-thread mutex.
    void on_wan_change(WanCallback cb);

    /// Default ds-server socket path. Duplicated from
    /// data_store::proto::kDefaultSocketPath so this header stays
    /// free of data_store/ includes.
    static const char* kDefaultSocketPath;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string           m_path;
    bool                  m_ok = false;
};

} // namespace openvpn_client

#endif /* __openvpn_client_ds_bridge_hpp__ */
