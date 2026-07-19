#ifndef __cellular_qmi_wan_hpp__
#define __cellular_qmi_wan_hpp__

#include <string>
#include <vector>

/**
 * @file qmi_wan.hpp
 * @brief Pure parsers for the `qmicli` output the DirectIP data-call supervisor
 *        needs.
 *
 * With the WP7702 in the DirectIP USB composition (ECM dropped via
 * `AT!USBCOMP=1,1,0000010D`), the host owns the cellular data call on `wwan0`
 * over QMI. iot-cellular-client shells out to `qmicli` to start/monitor the
 * bearer; these free functions turn its text output into typed values. They
 * touch no hardware or subprocess — fully host-unit-testable. The daemon does
 * the `qmicli` exec + sysfs/`ip` plumbing and feeds the captured stdout here.
 *
 * See apps/docs/hw-bringup-wp7702-cellular-wan.md §4.4.
 */

namespace cellular {

/// IPv4 settings from `qmicli --wds-get-current-settings`.
struct DirectIpSettings {
    bool                     valid = false;   ///< true once an IPv4 address parsed
    std::string              ip;              ///< e.g. "100.115.65.218"
    std::string              gateway;         ///< e.g. "100.115.65.217"
    std::string              subnet_mask;     ///< e.g. "255.255.255.252"
    int                      prefix = 0;      ///< CIDR prefix derived from mask (0 = unknown)
    std::vector<std::string> dns;             ///< primary, then secondary (IPv4 only)
    int                      mtu = 0;         ///< 0 = unknown
};

/// Outcome of `qmicli --wds-start-network`.
struct StartResult {
    enum class Status {
        Started,        ///< "Network started" — bearer up
        CidTimeout,     ///< "CID allocation failed … Transaction timed out"
                        ///< → the qmi-proxy is wedged; kill it and retry
        CallFailed,     ///< "couldn't start network: … CallFailed" (network side)
        OtherError,     ///< any other "error:" line
        Unknown,        ///< nothing recognised (empty / truncated output)
    };
    Status      status = Status::Unknown;
    std::string handle;        ///< "Packet data handle" on success
    std::string end_reason;    ///< "call end reason (…)" text on CallFailed
    std::string verbose;       ///< "verbose call end reason (…)" text on CallFailed
};

/// Parse `qmicli --wds-get-current-settings` stdout.
DirectIpSettings parse_current_settings(const std::string& out);

/// Parse `qmicli --wds-start-network …` stdout+stderr (pass both, joined).
StartResult parse_start_network(const std::string& out);

/// True when `qmicli --nas-get-serving-system` reports `PS: 'attached'`.
/// The PS (packet-switched) domain must be attached before a data call can
/// start; CS-registration alone is not enough (see §4.4 — PS can detach while
/// the modem stays CS-registered, and `--wds-start-network` then CallFails).
bool parse_ps_attached(const std::string& serving_system_out);

/// True when `qmicli --wds-get-packet-service-status` reports
/// `Connection status: 'connected'`.
bool parse_connected(const std::string& packet_service_status_out);

/// Dotted IPv4 netmask ("255.255.255.252") → CIDR prefix (30). 0 if unparseable.
int mask_to_prefix(const std::string& mask);

} // namespace cellular

#endif /*__cellular_qmi_wan_hpp__*/
