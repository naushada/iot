#ifndef __ddns_client_hpp__
#define __ddns_client_hpp__

#include <memory>
#include <mutex>
#include <string>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"
#include "ddns/provider.hpp"
#include "ddns/public_ip.hpp"

/**
 * @file ddns_client.hpp
 * @brief iot-ddnsd — reactor-driven device-side Dynamic DNS updater.
 *
 * Connects to ds, loads the ddns.* config + secrets, watches the config keys
 * plus the active WAN IP, and on a periodic ACE timer detects the device's
 * current public IPv4 and (when it changed, or a forced-refresh window elapsed)
 * updates the configured DNS A record through the selected ProviderBackend.
 * All I/O is ACE; the outbound HTTPS (curl via ACE_Process) blocks the tick,
 * like cellular-client's synchronous modem poll. Ships disabled by default.
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
    Ok,             ///< record published + (heuristically) reachable
    OkUnreachable,  ///< published, but the IP is CGNAT/private (not reachable)
    Error,          ///< last detect/update failed (see ddns.last.error)
};

const char* to_string(State s);

class DdnsClient : public ACE_Event_Handler {
public:
    struct Config {
        std::string  ds_sock;
        bool         enabled           = false;
        std::string  provider          = "dyndns2";
        std::string  hostname;
        unsigned     interval_sec      = 300;
        unsigned     refresh_force_sec = 86400;
        std::string  ip_source         = "echo";
        std::string  token_path;                 ///< credential-file override

        // provider targets (non-secret)
        std::string  dyndns2_server = "members.dyndns.org";
        std::string  dyndns2_user;
        std::string  duckdns_domains;
        std::string  cf_zone_id;
        std::string  r53_zone_id;
        std::string  r53_access_key;

        // secrets (read from write-only ds keys or the credential file)
        std::string  dyndns2_token;
        std::string  duckdns_token;
        std::string  cf_token;
        std::string  r53_secret_key;
    };

    explicit DdnsClient(Config cfg) : m_cfg(std::move(cfg)) {}
    ~DdnsClient() override = default;

    int run();
    int handle_timeout(const ACE_Time_Value&, const void*) override;
    int handle_exception(ACE_HANDLE = ACE_INVALID_HANDLE) override;

private:
    void load_config_from_ds();
    void publish_state(State s);
    void set_error(const std::string& msg);
    void on_config_event(const data_store::Client::Event& ev);
    void reload();

    void tick();                        ///< detect + (maybe) update
    Creds creds_for_provider() const;   ///< map ds config → backend Creds
    static bool looks_reachable(const std::string& ip);  ///< CGNAT/private heuristic

    Config              m_cfg;
    data_store::Client  m_ds;
    State               m_state = State::Disabled;

    std::unique_ptr<PublicIpDetector>  m_detector;
    std::unique_ptr<ProviderBackend>   m_backend;
    std::string                        m_backend_provider;   ///< which provider m_backend is

    std::string m_last_ip;        ///< last IP we published
    long        m_last_ok_ts = 0; ///< epoch of last successful update
    long        m_last_push_ts = 0; ///< epoch of last provider call (for force-refresh)
    unsigned    m_version = 0;

    std::mutex  m_mtx;
    bool        m_cfg_dirty = false;
};

} // namespace ddns

#endif /* __ddns_client_hpp__ */
