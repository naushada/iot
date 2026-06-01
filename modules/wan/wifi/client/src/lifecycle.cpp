#include "lifecycle.hpp"

namespace wifi_client {

void Lifecycle::transition(std::string_view to) {
    if (m_state == to) return;          // NFR-WIFI-004
    m_state = to;
    if (m_sinks.set_state) m_sinks.set_state(m_state);
}

void Lifecycle::step(const ctrl::CtrlEvent& ev) {
    using K = ctrl::CtrlEvent::Kind;

    switch (ev.kind) {
    case K::ScanStarted:
        // Allowed from any non-terminal state: re-scans happen on a
        // periodic timer or operator bump even while connected
        // (kernel-side roaming hasn't been requested by the daemon
        // in v1; if scanning while connected becomes a problem we
        // gate it here).
        if (m_state == state::kDisconnected || m_state == state::kExited) {
            transition(state::kScanning);
        }
        break;

    case K::ScanResults:
        // Don't change state on raw scan-results — Supervisor will
        // publish the JSON; FSM just notifies.
        if (m_sinks.on_scan_results) m_sinks.on_scan_results();
        break;

    case K::Connected:
        // wpa_supplicant emits CONNECTED only after the 4-way
        // handshake completes. We collapse the intermediate
        // "associating" / "4way" states into a single transition
        // because wpa_supplicant doesn't emit a distinct event for
        // them — we only see SCAN-RESULTS → CONNECTED.
        transition(state::kConnected);
        if (m_sinks.on_connected) m_sinks.on_connected(ev.ssid, ev.bssid);
        break;

    case K::Disconnected:
        transition(state::kDisconnected);
        if (m_sinks.on_disconnected) m_sinks.on_disconnected(ev.reason);
        break;

    case K::AssocReject:
        if (m_sinks.on_reject) {
            m_sinks.on_reject("assoc_reject:" + ev.reason);
        }
        transition(state::kScanning);
        break;

    case K::AuthReject:
        if (m_sinks.on_reject) {
            m_sinks.on_reject("auth_reject:" + ev.reason);
        }
        transition(state::kScanning);
        break;

    case K::Terminating:
        transition(state::kExited);
        if (m_sinks.on_disconnected) m_sinks.on_disconnected("terminating");
        break;

    case K::Unknown:
    default:
        break;
    }
}

} // namespace wifi_client
