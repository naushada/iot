#ifndef __wifi_client_supervisor_hpp__
#define __wifi_client_supervisor_hpp__

/// Supervisor — impure wiring for the wifi-client daemon (L15/D6).
///
/// Owns the four moving parts (DsBridge, ctrl::Client, two Process
/// instances for wpa_supplicant + DHCP, Lifecycle FSM) and drives
/// them through a single event loop:
///
///   1. Spawn wpa_supplicant with a freshly-generated .conf.
///   2. Open the ctrl socket; send ATTACH.
///   3. Outer loop:
///        - Poll ctrl for events with ≤200 ms timeout (NFR-WIFI-001).
///        - Check DsBridge for wifi.networks / wifi.scan.request
///          changes (via the DsBridge::on_change listener thread
///          which sets a flag).
///        - Apply: SCAN on bump, RECONFIGURE on additive change,
///          full restart on non-additive change.
///        - On Lifecycle::Connected → spawn DHCP child.
///        - On Lifecycle::Disconnected → reap DHCP.
///   4. SIGTERM cleanup: reap both children via Process destructor.
///
/// The CLASS itself does I/O; pure helpers live alongside in this
/// header so unit tests can verify the logic without standing up a
/// full daemon. The integration ("Supervisor actually drives
/// wpa_supplicant through to wifi.assoc.state='connected'") is
/// exercised by log/L15/smoke.sh against fake-wpa.sh (D8).

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ds_bridge.hpp"
#include "ctrl.hpp"
#include "lifecycle.hpp"
#include "process.hpp"

namespace data_store { class ServiceGate; }   // forward decl

namespace wifi_client {

// ─────────────────────── Pure helpers ─────────────────────────

/// One row in a parsed CTRL-EVENT-SCAN-RESULTS payload.
struct ScanEntry {
    std::string ssid;
    std::string bssid;
    int         signal = 0;        ///< dBm (negative)
    std::string flags;
};

/// Parse wpa_supplicant's SCAN_RESULTS reply text into rows. The
/// reply format is one row per line, header on the first line:
///
///   bssid / frequency / signal level / flags / ssid
///   aa:bb:cc:dd:ee:ff\t2412\t-52\t[WPA2-PSK-CCMP][ESS]\tHomeAP
///
/// Tab-separated. Empty or malformed lines are skipped. The
/// function is tolerant — the header is optionally detected by
/// non-numeric signal column.
std::vector<ScanEntry> parse_scan_results(const std::string& wpa_reply);

/// Sort by descending signal (strongest first), cap to `max_count`,
/// emit as JSON array of {ssid, bssid, signal, flags} per the
/// schema. NFR-WIFI-003.
std::string cap_and_serialize_scan_results(std::vector<ScanEntry> rows,
                                           std::size_t            max_count);

/// "Additive" classifier for a wifi.networks JSON change:
/// returns true when every entry in `before` is still present
/// in `after` (by ssid) with the same psk/key_mgmt/priority,
/// i.e. only adds happened. Reorder counts as non-additive
/// because operators express priority via the priority field,
/// not array order — but the conservative side of "is this safe
/// to RECONFIGURE vs needs full restart?" lives here. Bad JSON
/// on either side returns false. REQ-WIFI-020.
bool is_networks_change_additive(const std::string& before_json,
                                 const std::string& after_json);

/// Pick the index in `parsed_networks` whose SSID matches the
/// strongest entry in `scan_results`. Returns nullopt when no
/// configured SSID is visible. Production-side wpa_supplicant
/// does this internally via SELECT_NETWORK / priority — this
/// helper exists so the unit test can ratify the contract from
/// the daemon's POV.
std::optional<std::size_t>
select_best_network(const std::vector<WifiNetwork>& parsed_networks,
                    const std::vector<ScanEntry>&   scan_results);

/// Returns true if NetworkManager appears to be managing the
/// iface today: shells out to `systemctl is-active NetworkManager`
/// + checks for an existing /run/wpa_supplicant/<iface> socket.
/// Pure-string variant exists for tests; the default lookups the
/// host. REQ-WIFI-022.
bool nm_conflict_detected(const std::string& iface,
                          const std::string& ctrl_dir);

/// Injectable clock — Supervisor uses this for the 5-second RSSI
/// coalescing window (NFR-WIFI-002). Tests substitute a fake to
/// avoid wall-clock dependence (per RISK-WIFI-05).
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
};

class SystemClock : public Clock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
};

/// Holds a "last published" timestamp + value; returns true when
/// a new value SHOULD be published (5s elapsed since last, OR
/// value is materially different — currently "any change"). Pure
/// logic; the Supervisor calls it on every signal-poll reply.
class RssiCoalescer {
public:
    RssiCoalescer(Clock& clock, std::chrono::seconds window)
        : m_clock(clock), m_window(window) {}

    /// Returns true when the caller should publish `dbm`.
    bool should_publish(int dbm);

private:
    Clock&                                  m_clock;
    std::chrono::seconds                    m_window;
    std::optional<std::chrono::steady_clock::time_point> m_last;
    int                                     m_last_value = 0;
};

// ─────────────────────── Supervisor class ───────────────────

/// Run options for the daemon's event loop. Defaults match
/// production; smoke harnesses override `once` + the binary paths.
struct SupervisorOptions {
    std::string wpa_path     = "/usr/sbin/wpa_supplicant";
    std::string iface        = "wlan0";
    std::string ctrl_dir     = "/run/wpa_supplicant";
    /// `--once`: exit cleanly after the first CTRL-EVENT-CONNECTED.
    /// Used by smoke; production stays `false`.
    bool        once         = false;
};

class Supervisor {
public:
    Supervisor(DsBridge& ds, SupervisorOptions opt);
    ~Supervisor();

    Supervisor(const Supervisor&)            = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    /// Block on the calling thread until SIGTERM (or `once`
    /// completes). Returns when the outer loop exits cleanly.
    /// Two-deep return code: 0 success, 1 fatal (logged).
    int run();

private:
    DsBridge&             m_ds;
    SupervisorOptions     m_opt;
    SystemClock           m_clock;
    RssiCoalescer         m_rssi{m_clock, std::chrono::seconds(5)};

    Process               m_wpa;
    Process               m_dhcp;
    ctrl::Client          m_ctrl;
    Lifecycle             m_fsm;

    /// L16/D6 — services.wifi.client.enable gate. Composes with the
    /// NM-conflict gate; disable dominates conflict (the daemon
    /// returns "disabled" even if NM would otherwise refuse start).
    std::unique_ptr<data_store::ServiceGate> m_svc;
    std::thread                              m_svc_watcher;
    std::atomic<bool>                        m_svc_stop{false};

    /// One-shot startup: probe NM conflict, spawn wpa_supplicant,
    /// connect ctrl, ATTACH. Returns false on any fatal init
    /// failure (NM conflict, spawn fail, ctrl connect fail) — the
    /// daemon publishes the appropriate state and exits.
    bool initialize();
};

} // namespace wifi_client

#endif /* __wifi_client_supervisor_hpp__ */
