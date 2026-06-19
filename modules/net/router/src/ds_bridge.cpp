#include "ds_bridge.hpp"

#include <mutex>
#include <utility>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace net_router {

const char* DsBridge::kDefaultSocketPath = data_store::proto::kDefaultSocketPath;

namespace {

// Single source of truth for wire-level key strings.
constexpr const char* kTunDev          = "net.tun.dev";
constexpr const char* kLwM2MTargetIp   = "net.lwm2m.target.ip";
constexpr const char* kLwM2MTargetPort = "net.lwm2m.target.port";
constexpr const char* kIfacePriority   = "net.iface.priority";
constexpr const char* kIfaceEthName    = "net.iface.eth.name";
constexpr const char* kIfaceWifiName   = "net.iface.wifi.name";
constexpr const char* kIfaceCellName   = "net.iface.cellular.name";
constexpr const char* kForwardPorts    = "net.forward.ports";
constexpr const char* kCustomRules     = "net.custom.rules";
constexpr const char* kPollIntervalSec = "net.poll.interval.sec";

constexpr const char* kState               = "net.state";
constexpr const char* kTunIp               = "net.tun.ip";
constexpr const char* kTunGateway          = "net.tun.gateway";
constexpr const char* kIfaceActive         = "net.iface.active";
constexpr const char* kIfaceActiveIp        = "net.iface.active.ip";
constexpr const char* kRulesAppliedCount   = "net.rules.applied.count";
constexpr const char* kLastApplyUnix       = "net.last.apply.unix";

} // namespace

// ─────────────────────────── Impl ──────────────────────────────────

struct DsBridge::Impl {
    data_store::Client              client;
    data_store::Client::WatchHandle watch_handle =
        data_store::Client::kInvalidHandle;

    mutable std::mutex            mtx;
    // Snapshot — listener thread writes, accessors read.
    std::optional<std::string>    tun_dev;
    std::optional<std::string>    lwm2m_target_ip;
    std::optional<std::uint32_t>  lwm2m_target_port;
    std::optional<std::string>    iface_priority;
    std::optional<std::string>    iface_eth_name;
    std::optional<std::string>    iface_wifi_name;
    std::optional<std::string>    iface_cellular_name;
    std::optional<std::string>    forward_ports;
    std::optional<std::string>    custom_rules;
    std::optional<std::uint32_t>  poll_interval_sec;
    ChangeCallback                cb;
};

DsBridge::DsBridge(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [netr:%t] %M %N:%l data-store not "
                            "available at %C (%C); net-router cannot "
                            "start\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [netr:%t] %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));

    const std::vector<std::string> read_keys = {
        kTunDev, kLwM2MTargetIp, kLwM2MTargetPort,
        kIfacePriority, kIfaceEthName, kIfaceWifiName, kIfaceCellName,
        kForwardPorts, kCustomRules, kPollIntervalSec,
    };

    std::vector<data_store::Client::GetResult> got;
    auto gs = m_impl->client.get(read_keys, got);
    if (gs.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        for (auto& r : got) {
            if (!r.has_value) continue;
            if      (r.key == kTunDev)          m_impl->tun_dev             = data_store::to_string(r.value);
            else if (r.key == kLwM2MTargetIp)   m_impl->lwm2m_target_ip     = data_store::to_string(r.value);
            else if (r.key == kLwM2MTargetPort) m_impl->lwm2m_target_port   = data_store::to_uint32(r.value);
            else if (r.key == kIfacePriority)   m_impl->iface_priority      = data_store::to_string(r.value);
            else if (r.key == kIfaceEthName)    m_impl->iface_eth_name      = data_store::to_string(r.value);
            else if (r.key == kIfaceWifiName)   m_impl->iface_wifi_name     = data_store::to_string(r.value);
            else if (r.key == kIfaceCellName)   m_impl->iface_cellular_name = data_store::to_string(r.value);
            else if (r.key == kForwardPorts)    m_impl->forward_ports       = data_store::to_string(r.value);
            else if (r.key == kCustomRules)     m_impl->custom_rules        = data_store::to_string(r.value);
            else if (r.key == kPollIntervalSec) m_impl->poll_interval_sec   = data_store::to_uint32(r.value);
        }
    }

    auto ws = m_impl->client.watch(
        read_keys,
        [this](const data_store::Client::Event& ev) {
            std::optional<Key> changed;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                if      (ev.key == kTunDev)          { m_impl->tun_dev             = data_store::to_string(ev.value);   changed = Key::TunDev;            }
                else if (ev.key == kLwM2MTargetIp)   { m_impl->lwm2m_target_ip     = data_store::to_string(ev.value);   changed = Key::LwM2MTargetIp;     }
                else if (ev.key == kLwM2MTargetPort) { m_impl->lwm2m_target_port   = data_store::to_uint32(ev.value);   changed = Key::LwM2MTargetPort;   }
                else if (ev.key == kIfacePriority)   { m_impl->iface_priority      = data_store::to_string(ev.value);   changed = Key::IfacePriority;     }
                else if (ev.key == kIfaceEthName)    { m_impl->iface_eth_name      = data_store::to_string(ev.value);   changed = Key::IfaceEthName;      }
                else if (ev.key == kIfaceWifiName)   { m_impl->iface_wifi_name     = data_store::to_string(ev.value);   changed = Key::IfaceWifiName;     }
                else if (ev.key == kIfaceCellName)   { m_impl->iface_cellular_name = data_store::to_string(ev.value);   changed = Key::IfaceCellularName; }
                else if (ev.key == kForwardPorts)    { m_impl->forward_ports       = data_store::to_string(ev.value);   changed = Key::ForwardPorts;      }
                else if (ev.key == kCustomRules)     { m_impl->custom_rules        = data_store::to_string(ev.value);   changed = Key::CustomRules;       }
                else if (ev.key == kPollIntervalSec) { m_impl->poll_interval_sec   = data_store::to_uint32(ev.value);   changed = Key::PollIntervalSec;   }
            }
            if (!changed) return;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l net config key '%C' "
                                "changed — nft ruleset regen required on "
                                "next tick\n"),
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
                   ACE_TEXT("%D [netr:%t] %M %N:%l watch register failed: "
                            "%C — cache primed but hot-reload disabled\n"),
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

// ─────────────────────── Read accessors ────────────────────────────

std::optional<std::string> DsBridge::tun_dev() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->tun_dev;
}
std::optional<std::string> DsBridge::lwm2m_target_ip() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->lwm2m_target_ip;
}
std::optional<std::uint32_t> DsBridge::lwm2m_target_port() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->lwm2m_target_port;
}
std::optional<std::string> DsBridge::iface_priority() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->iface_priority;
}
std::optional<std::string> DsBridge::iface_eth_name() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->iface_eth_name;
}
std::optional<std::string> DsBridge::iface_wifi_name() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->iface_wifi_name;
}
std::optional<std::string> DsBridge::iface_cellular_name() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->iface_cellular_name;
}
std::optional<std::string> DsBridge::forward_ports() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->forward_ports;
}
std::optional<std::string> DsBridge::custom_rules() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->custom_rules;
}
std::optional<std::uint32_t> DsBridge::poll_interval_sec() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->poll_interval_sec;
}

// ───────────────────────── Write side ──────────────────────────────

void DsBridge::set_state(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kState, s);
    if (!rs.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [netr:%t] %M %N:%l set %C='%C' failed: %C\n"),
                   kState, s.c_str(), rs.err.c_str()));
    }
}
void DsBridge::set_tun_ip(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kTunIp, s);
}
void DsBridge::set_tun_gateway(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kTunGateway, s);
}
void DsBridge::set_iface_active(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kIfaceActive, s);
}
void DsBridge::set_iface_active_ip(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kIfaceActiveIp, s);
}
void DsBridge::set_rules_applied_count(std::uint32_t n) {
    if (!m_ok) return;
    m_impl->client.set(kRulesAppliedCount, n);
}
void DsBridge::set_last_apply_unix(std::uint32_t t) {
    if (!m_ok) return;
    m_impl->client.set(kLastApplyUnix, t);
}

data_store::Client* DsBridge::client() {
    if (!m_ok || !m_impl) return nullptr;
    return &m_impl->client;
}

void DsBridge::on_change(ChangeCallback cb) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    m_impl->cb = std::move(cb);
}

} // namespace net_router
