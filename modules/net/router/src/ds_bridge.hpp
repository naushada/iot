#ifndef __net_router_ds_bridge_hpp__
#define __net_router_ds_bridge_hpp__

/// Bridge between net-router and ds-server.
///
/// Same pattern as modules/openvpn/client/src/ds_bridge.hpp (L12/D3):
/// owns the data_store::Client for the daemon lifetime, primes a
/// thread-safe snapshot of the readable net.* keys on startup,
/// registers a callback-style watch so subsequent ds-cli mutations
/// land in the cache and fire on_change.
///
/// First-cut hot-reload policy (matches L12 R4): log "rule regen
/// required" on any read-key change. Live regenerate + nft -f -
/// apply lands in D6 once the lifecycle FSM exists.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace data_store { class Client; }   // forward decl

namespace net_router {

class DsBridge {
public:
    enum class Key {
        TunDev,
        LwM2MTargetIp,
        LwM2MTargetPort,
        IfacePriority,
        IfaceEthName,
        IfaceWifiName,
        IfaceCellularName,
        ForwardPorts,
        CustomRules,
        PollIntervalSec,
    };

    using ChangeCallback = std::function<void(Key)>;

    explicit DsBridge(std::string socketPath = {});
    ~DsBridge();

    DsBridge(const DsBridge&)            = delete;
    DsBridge& operator=(const DsBridge&) = delete;

    bool connected() const { return m_ok; }
    const std::string& socket_path() const { return m_path; }

    // ─────────── Read snapshots ───────────
    std::optional<std::string>   tun_dev()              const;
    std::optional<std::string>   lwm2m_target_ip()      const;
    std::optional<std::uint32_t> lwm2m_target_port()    const;
    std::optional<std::string>   iface_priority()       const;
    std::optional<std::string>   iface_eth_name()       const;
    std::optional<std::string>   iface_wifi_name()      const;
    std::optional<std::string>   iface_cellular_name()  const;
    std::optional<std::string>   forward_ports()        const;
    std::optional<std::string>   custom_rules()         const;
    std::optional<std::uint32_t> poll_interval_sec()    const;

    /// Returns nullopt if net.lwm2m.target_ip is present; otherwise
    /// returns the one-element list {"net.lwm2m.target_ip"} so the
    /// caller's missing-required diagnostic is uniform with the
    /// other DsBridges.
    std::optional<std::vector<std::string>> missing_required() const;

    // ─────────── Write side ───────────
    void set_state(const std::string& s);
    void set_tun_ip(const std::string& s);
    void set_tun_gateway(const std::string& s);
    void set_iface_active(const std::string& s);
    void set_rules_applied_count(std::uint32_t n);
    void set_last_apply_unix(std::uint32_t t);

    void on_change(ChangeCallback cb);

    /// Access the underlying data_store::Client. Used by the L16
    /// ServiceGate (and any future shared helper) to share one
    /// listener thread + one socket connection across both the
    /// net.* watch and the services.net.router.enable watch.
    /// nullptr when !connected().
    data_store::Client* client();

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

} // namespace net_router

#endif /* __net_router_ds_bridge_hpp__ */
