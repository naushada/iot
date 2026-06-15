#include "ds_bridge.hpp"

#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace wifi_client {

const char* DsBridge::kDefaultSocketPath = data_store::proto::kDefaultSocketPath;

namespace {

// ─────────────────────── Key constants ──────────────────────────────
// Single source of truth for the wire-level key strings. Same names
// as main_impl.cpp's kReadKeys / kWriteKeys and schemas/wifi.lua so
// the daemon, the schema, and the test suite can't drift apart.
constexpr const char* kIface             = "wifi.iface";
constexpr const char* kCtrlDir           = "wifi.ctrl.dir";
constexpr const char* kWpaPath           = "wifi.wpa.path";
constexpr const char* kNetworks          = "wifi.networks";
constexpr const char* kScanIntervalSec   = "wifi.scan.interval.sec";
constexpr const char* kScanMaxResults    = "wifi.scan.max.results";
constexpr const char* kScanRequest       = "wifi.scan.request";
constexpr const char* kDhcpClient        = "wifi.dhcp.client";
constexpr const char* kDhcpPath          = "wifi.dhcp.path";

constexpr const char* kAssocState        = "wifi.assoc.state";
constexpr const char* kAssocSsid         = "wifi.assoc.ssid";
constexpr const char* kAssocBssid        = "wifi.assoc.bssid";
constexpr const char* kSignalRssi        = "wifi.signal.rssi";
constexpr const char* kScanResults       = "wifi.scan.results";
constexpr const char* kScanLastUnix      = "wifi.scan.last.unix";
constexpr const char* kDhcpState         = "wifi.dhcp.state";
constexpr const char* kDhcpIp            = "wifi.dhcp.ip";
constexpr const char* kPidWpa            = "wifi.pid.wpa";
constexpr const char* kPidDhcp           = "wifi.pid.dhcp";
constexpr const char* kLastError         = "wifi.last.error";

} // namespace

// ───────────────────────────── Impl ─────────────────────────────────

struct DsBridge::Impl {
    data_store::Client              client;
    data_store::Client::WatchHandle watch_handle =
        data_store::Client::kInvalidHandle;

    mutable std::mutex            mtx;
    // Snapshots updated by the listener thread + initial prime.
    std::optional<std::string>    iface;
    std::optional<std::string>    ctrl_dir;
    std::optional<std::string>    wpa_path;
    std::optional<std::string>    networks;
    std::optional<std::uint32_t>  scan_interval_sec;
    std::optional<std::uint32_t>  scan_max_results;
    std::optional<std::uint32_t>  scan_request;
    std::optional<std::string>    dhcp_client;
    std::optional<std::string>    dhcp_path;
    ChangeCallback                cb;
};

// ─────────────────────── ctor / dtor ────────────────────────────────

DsBridge::DsBridge(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l data-store not available "
                            "at %C (%C); wifi-client cannot start\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [wifi:%t] %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));

    // Prime the snapshot with one initial get. Every read key has a
    // schema default (per schemas/wifi.lua) so the cache is fully
    // populated as soon as ds-server returns. wifi.networks defaults
    // to a placeholder PSK network (ssid "changeme"); the Supervisor
    // tries to associate and parks in disconnected until that AP is
    // found or an operator overrides wifi.networks. An operator-set
    // empty "[]" is treated as "no networks configured".
    const std::vector<std::string> read_keys = {
        kIface, kCtrlDir, kWpaPath, kNetworks,
        kScanIntervalSec, kScanMaxResults, kScanRequest,
        kDhcpClient, kDhcpPath,
    };
    std::vector<data_store::Client::GetResult> got;
    auto gs = m_impl->client.get(read_keys, got);
    if (gs.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        for (auto& r : got) {
            if (!r.has_value) continue;
            if      (r.key == kIface)            m_impl->iface             = data_store::to_string(r.value);
            else if (r.key == kCtrlDir)          m_impl->ctrl_dir          = data_store::to_string(r.value);
            else if (r.key == kWpaPath)          m_impl->wpa_path          = data_store::to_string(r.value);
            else if (r.key == kNetworks)         m_impl->networks          = data_store::to_string(r.value);
            else if (r.key == kScanIntervalSec)  m_impl->scan_interval_sec = data_store::to_uint32(r.value);
            else if (r.key == kScanMaxResults)   m_impl->scan_max_results  = data_store::to_uint32(r.value);
            else if (r.key == kScanRequest)      m_impl->scan_request      = data_store::to_uint32(r.value);
            else if (r.key == kDhcpClient)       m_impl->dhcp_client       = data_store::to_string(r.value);
            else if (r.key == kDhcpPath)         m_impl->dhcp_path         = data_store::to_string(r.value);
        }
    }

    // Register the watch. Same 9 keys; the listener thread routes by
    // string match into the snapshot + the per-key Key enum the
    // caller's on_change handler sees.
    auto ws = m_impl->client.watch(
        read_keys,
        [this](const data_store::Client::Event& ev) {
            std::optional<Key> changed;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                if      (ev.key == kIface)           { m_impl->iface             = data_store::to_string(ev.value);   changed = Key::Iface;           }
                else if (ev.key == kCtrlDir)         { m_impl->ctrl_dir          = data_store::to_string(ev.value);   changed = Key::CtrlDir;         }
                else if (ev.key == kWpaPath)         { m_impl->wpa_path          = data_store::to_string(ev.value);   changed = Key::WpaPath;         }
                else if (ev.key == kNetworks)        { m_impl->networks          = data_store::to_string(ev.value);   changed = Key::Networks;        }
                else if (ev.key == kScanIntervalSec) { m_impl->scan_interval_sec = data_store::to_uint32(ev.value); changed = Key::ScanIntervalSec; }
                else if (ev.key == kScanMaxResults)  { m_impl->scan_max_results  = data_store::to_uint32(ev.value); changed = Key::ScanMaxResults;  }
                else if (ev.key == kScanRequest)     { m_impl->scan_request      = data_store::to_uint32(ev.value); changed = Key::ScanRequest;     }
                else if (ev.key == kDhcpClient)      { m_impl->dhcp_client       = data_store::to_string(ev.value);   changed = Key::DhcpClient;      }
                else if (ev.key == kDhcpPath)        { m_impl->dhcp_path         = data_store::to_string(ev.value);   changed = Key::DhcpPath;        }
            }
            if (!changed) return;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [wifi:%t] %M %N:%l config key "
                                "'%C' changed\n"),
                       ev.key.c_str()));
            ChangeCallback cbcopy;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                cbcopy = m_impl->cb;
            }
            if (cbcopy) cbcopy(*changed);
        },
        &m_impl->watch_handle);
    if (!ws.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l watch register failed: "
                            "%C — cache primed but change notifications "
                            "disabled\n"),
                   ws.err.c_str()));
    }
}

DsBridge::~DsBridge() {
    if (!m_impl) return;
    if (m_impl->watch_handle != data_store::Client::kInvalidHandle) {
        m_impl->client.unwatch(m_impl->watch_handle, /*timeout_ms=*/200);
    }
    m_impl->client.close();
}

// ─────────────────────── Read accessors ─────────────────────────────

std::optional<std::string> DsBridge::iface() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->iface;
}
std::optional<std::string> DsBridge::ctrl_dir() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->ctrl_dir;
}
std::optional<std::string> DsBridge::wpa_path() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->wpa_path;
}
std::optional<std::string> DsBridge::networks() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->networks;
}
std::optional<std::uint32_t> DsBridge::scan_interval_sec() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->scan_interval_sec;
}
std::optional<std::uint32_t> DsBridge::scan_max_results() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->scan_max_results;
}
std::optional<std::uint32_t> DsBridge::scan_request() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->scan_request;
}
std::optional<std::string> DsBridge::dhcp_client() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dhcp_client;
}
std::optional<std::string> DsBridge::dhcp_path() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dhcp_path;
}

std::optional<std::vector<std::string>>
DsBridge::missing_required() const {
    if (!m_ok) {
        // Not connected → surface placeholder list so the caller's
        // "missing config" diagnostic path is uniform with
        // openvpn-client's shape.
        return std::vector<std::string>{kIface};
    }
    // Schema defaults make every read key satisfied at this layer.
    // wifi.networks shape validation lives in the Supervisor.
    return std::nullopt;
}

// ─────────────────────── Write side ─────────────────────────────────

namespace {
void log_set_failure(const char* key, const data_store::Status& rs) {
    ACE_ERROR((LM_WARNING,
               ACE_TEXT("%D [wifi:%t] %M %N:%l set %C failed: %C\n"),
               key, rs.err.c_str()));
}
} // namespace

void DsBridge::set_assoc_state(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kAssocState, s);
    if (!rs.ok) log_set_failure(kAssocState, rs);
}
void DsBridge::set_assoc_ssid(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kAssocSsid, s);
    if (!rs.ok) log_set_failure(kAssocSsid, rs);
}
void DsBridge::set_assoc_bssid(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kAssocBssid, s);
    if (!rs.ok) log_set_failure(kAssocBssid, rs);
}
void DsBridge::set_signal_rssi(std::int32_t dbm) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kSignalRssi, dbm);
    if (!rs.ok) log_set_failure(kSignalRssi, rs);
}
void DsBridge::set_scan_results(const std::string& json) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kScanResults, json);
    if (!rs.ok) log_set_failure(kScanResults, rs);
}
void DsBridge::set_scan_last_unix(std::uint32_t unix_ts) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kScanLastUnix, unix_ts);
    if (!rs.ok) log_set_failure(kScanLastUnix, rs);
}
void DsBridge::set_dhcp_state(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kDhcpState, s);
    if (!rs.ok) log_set_failure(kDhcpState, rs);
}
void DsBridge::set_dhcp_ip(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kDhcpIp, s);
    if (!rs.ok) log_set_failure(kDhcpIp, rs);
}
void DsBridge::set_pid_wpa(std::uint32_t p) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kPidWpa, p);
    if (!rs.ok) log_set_failure(kPidWpa, rs);
}
void DsBridge::set_pid_dhcp(std::uint32_t p) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kPidDhcp, p);
    if (!rs.ok) log_set_failure(kPidDhcp, rs);
}
void DsBridge::set_last_error(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kLastError, s);
    if (!rs.ok) log_set_failure(kLastError, rs);
}

// ─────────────────────── on_change registration ─────────────────────

data_store::Client* DsBridge::client() {
    if (!m_ok || !m_impl) return nullptr;
    return &m_impl->client;
}

void DsBridge::on_change(ChangeCallback cb) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    m_impl->cb = std::move(cb);
}

} // namespace wifi_client
