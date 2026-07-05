#ifndef __ddns_client_hpp__
#define __ddns_client_hpp__

#include <mutex>
#include <string>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"

/**
 * @file ddns_client.hpp
 * @brief iot-ddnsd — reactor-driven device-side Dynamic DNS updater.
 *
 * Connects to ds, loads the ddns.* config, watches the config keys plus the
 * active WAN IP, and on a periodic ACE timer detects the device's current
 * public IPv4 and (when it changed, or a forced-refresh window elapsed) updates
 * the configured DNS A record through the selected ProviderBackend. All I/O is
 * ACE; no raw POSIX. Ships disabled by default (ddns.enabled=false).
 *
 * PR-1 (this file) is the skeleton: config load + watch + timer + state
 * publish. Public-IP detection (FR-3) and provider backends (FR-4..FR-7) are
 * wired in by later PRs; this tick logs the resolved config and target.
 *
 * See apps/docs/tdd-ddns.md.
 */

namespace ddns {

/// Lifecycle/health token published to ddns.state (device-ui renders it).
enum class State {
    Disabled,       ///< ddns.enabled=false
    WaitingClock,   ///< HTTPS needs a valid clock (NTP-no-RTC cold window)
    Detecting,      ///< resolving the current public IP
    Updating,       ///< calling the provider
    Ok,             ///< record published + (optionally) reachable
    OkUnreachable,  ///< published, but the IP looks un-routable (CGNAT)
    Error,          ///< last detect/update failed (see ddns.last.error)
};

const char* to_string(State s);

class DdnsClient : public ACE_Event_Handler {
public:
    struct Config {
        std::string  ds_sock;             ///< "" → ds default socket
        // The tunables below are argv defaults; ds ddns.* keys override them at
        // load time and on live change.
        bool         enabled       = false;
        std::string  provider      = "dyndns2";
        std::string  hostname;
        unsigned     interval_sec  = 300;
        unsigned     refresh_force_sec = 86400;
        std::string  ip_source     = "echo";
    };

    explicit DdnsClient(Config cfg) : m_cfg(std::move(cfg)) {}
    ~DdnsClient() override = default;

    /// Connect ds, load config, register the watch + timer, run the reactor.
    /// Returns a process exit code (non-zero → systemd restarts us).
    int run();

    /// Periodic tick: detect public IP, update the record when needed.
    int handle_timeout(const ACE_Time_Value&, const void*) override;
    /// Reactor wakeup from a ddns.*/net.* ds watch (fires on the listener
    /// thread → we only flag dirty + notify(); the reload runs reactor-side).
    int handle_exception(ACE_HANDLE = ACE_INVALID_HANDLE) override;

private:
    void load_config_from_ds();
    void publish_state(State s);
    void set_error(const std::string& msg);
    void on_config_event(const data_store::Client::Event& ev);
    void reload();

    Config              m_cfg;
    data_store::Client  m_ds;
    State               m_state = State::Disabled;
    std::string         m_last_ip;
    long                m_last_ok_ts = 0;
    unsigned            m_version = 0;

    // ds watch fires on the listener thread; it records dirty + notify()s the
    // reactor, and reload() runs on the reactor thread (handle_exception).
    std::mutex          m_mtx;
    bool                m_cfg_dirty = false;
};

} // namespace ddns

#endif /* __ddns_client_hpp__ */
