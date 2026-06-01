#ifndef __wifi_client_lifecycle_hpp__
#define __wifi_client_lifecycle_hpp__

/// Pure wifi-client lifecycle FSM (L15/D6).
///
/// Maps CtrlEvent into wifi.assoc.state transitions + side-effect
/// callbacks via Sinks. No sockets, no Process, no DsBridge —
/// production wires real callables, tests substitute lambdas
/// capturing into a struct (same shape openvpn-client's
/// lifecycle_test uses).

#include <functional>
#include <string>
#include <string_view>

#include "ctrl.hpp"

namespace wifi_client {

/// String value the daemon writes into wifi.assoc.state. Strings
/// not an enum because the schema-side value is a string, and
/// keeping one definition avoids drift.
namespace state {
constexpr std::string_view kDisconnected = "disconnected";
constexpr std::string_view kScanning     = "scanning";
constexpr std::string_view kAssociating  = "associating";
constexpr std::string_view kFourWay      = "4way";
constexpr std::string_view kConnected    = "connected";
constexpr std::string_view kConflict     = "conflict";
constexpr std::string_view kExited       = "exited";
} // namespace state

class Lifecycle {
public:
    /// Sinks the FSM calls into when transitions / events warrant.
    /// Production fills these from a Supervisor; tests substitute
    /// capture-lambdas to make assertions on call ordering +
    /// values.
    struct Sinks {
        /// Called on every assoc-state transition (suppressed when
        /// the new state == the previous one — REQ-WIFI-018 +
        /// NFR-WIFI-004).
        std::function<void(std::string_view /*new_state*/)>            set_state;
        /// Called on CTRL-EVENT-CONNECTED. Args: ssid (may be
        /// empty when id_str= is empty), bssid.
        std::function<void(const std::string&, const std::string&)>    on_connected;
        /// Called on CTRL-EVENT-DISCONNECTED / Terminating.
        std::function<void(const std::string& /*reason*/)>             on_disconnected;
        /// Called on CTRL-EVENT-SCAN-RESULTS — Supervisor issues
        /// SCAN_RESULTS, processes, writes wifi.scan.results.
        std::function<void()>                                          on_scan_results;
        /// Called on CTRL-EVENT-ASSOC-REJECT / AUTH-REJECT. Args:
        /// "reject_type:reason_code" (Supervisor writes to
        /// wifi.last.error and stays in scanning).
        std::function<void(const std::string& /*err*/)>                on_reject;
    };

    explicit Lifecycle(Sinks sinks) : m_sinks(std::move(sinks)) {}

    /// Consume one parsed CtrlEvent. Writes to sinks on any
    /// state transition. Idempotent under same-event repeats:
    /// repeated Connected events don't re-fire on_connected nor
    /// re-publish state="connected".
    void step(const ctrl::CtrlEvent& ev);

    /// The state the FSM is currently in. Public so the
    /// Supervisor can decide whether to spawn DHCP, log a state
    /// dump, etc.
    std::string_view current() const { return m_state; }

private:
    Sinks m_sinks;
    std::string_view m_state = state::kDisconnected;

    /// Publish a new state via Sinks::set_state, but only when
    /// the value is actually changing. NFR-WIFI-004.
    void transition(std::string_view to);
};

} // namespace wifi_client

#endif /* __wifi_client_lifecycle_hpp__ */
