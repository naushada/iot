#ifndef __wifi_client_ds_bridge_hpp__
#define __wifi_client_ds_bridge_hpp__

/// Bridge between wifi-client and ds-server.
///
/// Mirrors modules/openvpn/client/src/ds_bridge.hpp for the wifi.*
/// namespace. Holds a data_store::Client for the daemon lifetime;
/// primes a thread-safe snapshot of the readable keys on startup;
/// registers a callback-style watch so subsequent `ds-cli set wifi.*`
/// mutations land in the cache and fire the caller's on_change handler.
///
/// L15/D3 — first-cut hot-reload policy: the watch logs the change
/// and fires the caller's on_change handler with a typed Key enum.
/// Live re-application (regenerating wpa_supplicant.conf, RECONFIGURE
/// vs full restart) is the Supervisor's responsibility (D6).
///
/// Threading: all read accessors and missing_required() are
/// thread-safe (internal mutex). The data_store::Client's listener
/// thread is the source of cache updates; main-thread callers take
/// the same mutex for read.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wifi_client {

class DsBridge {
public:
    /// Which key did the on_change() callback fire for? Exposing
    /// the raw string would work too, but an enum makes
    /// switch-statements in the consumer exhaustive-warnings-friendly.
    enum class Key {
        Iface,
        CtrlDir,
        WpaPath,
        Networks,
        ScanIntervalSec,
        ScanMaxResults,
        ScanRequest,
        DhcpClient,
        DhcpPath,
    };

    using ChangeCallback = std::function<void(Key)>;

    /// Connect to `socketPath` (empty → default
    /// /var/run/iot/data_store.sock), prime the snapshot, register
    /// the watch. Connection failures are silently absorbed — call
    /// connected() to check before exercising the accessors.
    explicit DsBridge(std::string socketPath = {});
    ~DsBridge();

    DsBridge(const DsBridge&)            = delete;
    DsBridge& operator=(const DsBridge&) = delete;

    bool connected() const { return m_ok; }
    const std::string& socket_path() const { return m_path; }

    // ─────────────────────── Read snapshots ────────────────────────
    // Return whatever the listener thread last observed. nullopt
    // for keys that were never set AND have no schema default (the
    // wifi.* schema gives every read key a default, so accessors
    // mostly return a value as soon as connected() is true).
    std::optional<std::string>   iface()              const;
    std::optional<std::string>   ctrl_dir()           const;
    std::optional<std::string>   wpa_path()           const;
    std::optional<std::string>   networks()           const;  // raw JSON
    std::optional<std::uint32_t> scan_interval_sec()  const;
    std::optional<std::uint32_t> scan_max_results()   const;
    std::optional<std::uint32_t> scan_request()       const;
    std::optional<std::string>   dhcp_client()        const;
    std::optional<std::string>   dhcp_path()          const;

    /// Returns nullopt when the connection is healthy (every read
    /// key has a schema default, so there are no genuinely-missing
    /// keys at this layer — JSON validation of wifi.networks is
    /// the Supervisor's job, not the bridge's). When `connected()`
    /// is false the function returns a placeholder list so the
    /// caller's "ds unreachable" diagnostic path can be uniform
    /// with openvpn-client's missing_required() shape.
    std::optional<std::vector<std::string>> missing_required() const;

    // ─────────────────────── Write side ────────────────────────────
    // Each setter issues a data_store::Client::set on the shared
    // socket. Best-effort: a wire-level failure is logged via
    // ACE_ERROR but not surfaced — the caller's state machine has
    // already advanced.

    void set_assoc_state(const std::string& s);
    void set_assoc_ssid(const std::string& s);
    void set_assoc_bssid(const std::string& s);
    void set_signal_rssi(std::int32_t dbm);
    void set_scan_results(const std::string& json);
    void set_scan_last_unix(std::uint32_t unix_ts);
    void set_dhcp_state(const std::string& s);
    void set_dhcp_ip(const std::string& s);
    void set_pid_wpa(std::uint32_t p);
    void set_pid_dhcp(std::uint32_t p);
    void set_last_error(const std::string& s);

    /// Register a per-key change listener. Fires on the
    /// data_store::Client's listener thread — keep it short, don't
    /// block on locks the main thread might hold. Pass nullptr to
    /// clear a previous registration (REQ-WIFI-009).
    void on_change(ChangeCallback cb);

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

} // namespace wifi_client

#endif /* __wifi_client_ds_bridge_hpp__ */
