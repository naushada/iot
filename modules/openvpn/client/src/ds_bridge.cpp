#include "ds_bridge.hpp"

#include <ctime>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace openvpn_client {

const char* DsBridge::kDefaultSocketPath = data_store::proto::kDefaultSocketPath;

namespace {

// ─────────────────────── Key constants ──────────────────────────────
// Single source of truth for the wire-level key strings so accessors,
// the watch list, the listener-thread router, and missing_required()
// can't drift apart.
constexpr const char* kRemoteHost  = "vpn.remote.host";
constexpr const char* kRemotePort  = "vpn.remote.port";
constexpr const char* kRemoteProto = "vpn.remote.proto";
constexpr const char* kCertPath    = "vpn.cert.path";
constexpr const char* kKeyPath     = "vpn.key.path";
constexpr const char* kCaPath      = "vpn.ca.path";
constexpr const char* kCipher      = "vpn.cipher";
constexpr const char* kDev         = "vpn.dev";
constexpr const char* kMgmtPort    = "vpn.mgmt.port";

constexpr const char* kState           = "vpn.state";
constexpr const char* kAssignedIp      = "vpn.assigned.ip";
constexpr const char* kAssignedGateway = "vpn.assigned.gateway";
constexpr const char* kAssignedNetmask = "vpn.assigned.netmask";
constexpr const char* kAssignedDns     = "vpn.assigned.dns";
constexpr const char* kConnectedUnix   = "vpn.connected.unix";
constexpr const char* kPid             = "vpn.pid";
constexpr const char* kExitCode        = "vpn.exit_code";
constexpr const char* kGateReason      = "vpn.gate.reason";
constexpr const char* kBoundIface      = "vpn.bound.iface";

/// Single key out of the net.* namespace — the WAN gate. Read-only
/// from openvpn-client's perspective; net-router owns the write side.
constexpr const char* kNetIfaceActive  = "net.iface.active";

/// Variant→T lift with int32→uint32 promotion for non-negative ints
/// (schema's min=0 catches the negative case at set time so this is
/// just defensive). Otherwise nullopt for type mismatch.
template <class T>
std::optional<T> as(const data_store::Value& v) {
    if (auto* p = std::get_if<T>(&v)) return *p;
    if constexpr (std::is_same_v<T, std::uint32_t>) {
        if (auto* p = std::get_if<std::int32_t>(&v); p && *p >= 0) {
            return static_cast<std::uint32_t>(*p);
        }
    }
    return std::nullopt;
}

} // namespace

// ───────────────────────────── Impl ─────────────────────────────────

struct DsBridge::Impl {
    data_store::Client              client;
    data_store::Client::WatchHandle watch_handle =
        data_store::Client::kInvalidHandle;

    mutable std::mutex            mtx;
    // Snapshots updated by the listener thread + initial prime.
    std::optional<std::string>    remote_host;
    std::optional<std::uint32_t>  remote_port;
    std::optional<std::string>    remote_proto;
    std::optional<std::string>    cert_path;
    std::optional<std::string>    key_path;
    std::optional<std::string>    ca_path;
    std::optional<std::string>    cipher;
    std::optional<std::string>    dev;
    std::optional<std::uint32_t>  mgmt_port;
    std::optional<std::string>    wan_iface;
    ChangeCallback                cb;
    WanCallback                   wan_cb;
};

// ─────────────────────── ctor / dtor ────────────────────────────────

DsBridge::DsBridge(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l data-store not available "
                            "at %C (%C); openvpn-client cannot start\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));

    // Prime the snapshot with one initial get. Missing values stay
    // nullopt; the caller checks missing_required() before spawning
    // openvpn so a half-configured store can't start a half-broken
    // tunnel.
    const std::vector<std::string> read_keys = {
        kRemoteHost, kRemotePort, kRemoteProto,
        kCertPath, kKeyPath, kCaPath,
        kCipher, kDev, kMgmtPort,
        kNetIfaceActive,
    };
    std::vector<data_store::Client::GetResult> got;
    auto gs = m_impl->client.get(read_keys, got);
    if (gs.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        for (auto& r : got) {
            if (!r.has_value) continue;
            if      (r.key == kRemoteHost)     m_impl->remote_host  = data_store::to_string(r.value);
            else if (r.key == kRemotePort)     m_impl->remote_port  = data_store::to_uint32(r.value);
            else if (r.key == kRemoteProto)    m_impl->remote_proto = data_store::to_string(r.value);
            else if (r.key == kCertPath)       m_impl->cert_path    = data_store::to_string(r.value);
            else if (r.key == kKeyPath)        m_impl->key_path     = data_store::to_string(r.value);
            else if (r.key == kCaPath)         m_impl->ca_path      = data_store::to_string(r.value);
            else if (r.key == kCipher)         m_impl->cipher       = data_store::to_string(r.value);
            else if (r.key == kDev)            m_impl->dev          = data_store::to_string(r.value);
            else if (r.key == kMgmtPort)       m_impl->mgmt_port    = data_store::to_uint32(r.value);
            else if (r.key == kNetIfaceActive) {
                // net-router writes empty-string when no iface is up;
                // collapse to nullopt so the supervisor's "gated" state
                // has a single representation.
                auto s = data_store::to_string(r.value);
                if (s.has_value() && !s->empty()) m_impl->wan_iface = std::move(s);
            }
        }
    }

    // Register the watch. Same 9 keys; the listener thread routes by
    // string match into the snapshot + the per-key Key enum the
    // caller's on_change handler sees.
    auto ws = m_impl->client.watch(
        read_keys,
        [this](const data_store::Client::Event& ev) {
            // net.iface.active goes through its own callback path —
            // it has a different signature (snapshot vs Key enum) and
            // a different consumer (Supervisor's gate, not the
            // hot-reload logger).
            if (ev.key == kNetIfaceActive) {
                std::optional<std::string> snapshot;
                {
                    auto s = data_store::to_string(ev.value);
                    if (s.has_value() && !s->empty()) snapshot = std::move(s);
                    std::lock_guard<std::mutex> g(m_impl->mtx);
                    m_impl->wan_iface = snapshot;
                }
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D [ovpn:%t] %M %N:%l WAN gate event "
                                    "net.iface.active='%C'\n"),
                           snapshot.has_value() ? snapshot->c_str() : ""));
                WanCallback wcbcopy;
                {
                    std::lock_guard<std::mutex> g(m_impl->mtx);
                    wcbcopy = m_impl->wan_cb;
                }
                if (wcbcopy) wcbcopy(snapshot);
                return;
            }

            std::optional<Key> changed;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                if      (ev.key == kRemoteHost)  { m_impl->remote_host  = data_store::to_string(ev.value);   changed = Key::RemoteHost;  }
                else if (ev.key == kRemotePort)  { m_impl->remote_port  = data_store::to_uint32(ev.value); changed = Key::RemotePort;  }
                else if (ev.key == kRemoteProto) { m_impl->remote_proto = data_store::to_string(ev.value);   changed = Key::RemoteProto; }
                else if (ev.key == kCertPath)    { m_impl->cert_path    = data_store::to_string(ev.value);   changed = Key::CertPath;    }
                else if (ev.key == kKeyPath)     { m_impl->key_path     = data_store::to_string(ev.value);   changed = Key::KeyPath;     }
                else if (ev.key == kCaPath)      { m_impl->ca_path      = data_store::to_string(ev.value);   changed = Key::CaPath;      }
                else if (ev.key == kCipher)      { m_impl->cipher       = data_store::to_string(ev.value);   changed = Key::Cipher;      }
                else if (ev.key == kDev)         { m_impl->dev          = data_store::to_string(ev.value);   changed = Key::Dev;         }
                else if (ev.key == kMgmtPort)    { m_impl->mgmt_port    = data_store::to_uint32(ev.value); changed = Key::MgmtPort;    }
            }
            if (!changed) return;
            // Live hot-reload: the Supervisor's on_change handler tears the
            // current session down and respawns openvpn with the regenerated
            // config, so the new value (e.g. a cloud-pushed vpn.remote.host)
            // takes effect without a daemon restart.
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l vpn config key "
                                "'%C' changed — hot-reloading openvpn-client "
                                "(respawn with fresh config)\n"),
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
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l watch register failed: "
                            "%C — cache primed but hot-reload notifications "
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

std::optional<std::string> DsBridge::remote_host() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->remote_host;
}
std::optional<std::uint32_t> DsBridge::remote_port() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->remote_port;
}
std::optional<std::string> DsBridge::remote_proto() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->remote_proto;
}
std::optional<std::string> DsBridge::cert_path() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->cert_path;
}
std::optional<std::string> DsBridge::key_path() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->key_path;
}
std::optional<std::string> DsBridge::ca_path() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->ca_path;
}
std::optional<std::string> DsBridge::cipher() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->cipher;
}
std::optional<std::string> DsBridge::dev() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dev;
}
std::optional<std::uint32_t> DsBridge::mgmt_port() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->mgmt_port;
}
std::optional<std::string> DsBridge::wan_iface() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->wan_iface;
}

std::optional<std::vector<std::string>>
DsBridge::missing_required() const {
    if (!m_ok) {
        // Not even connected → everything is missing; surface as the
        // four required keys rather than the connection error so the
        // caller's "missing config" path is uniform.
        return std::vector<std::string>{
            kRemoteHost, kCertPath, kKeyPath, kCaPath};
    }
    std::vector<std::string> missing;
    {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        if (!m_impl->remote_host.has_value()) missing.emplace_back(kRemoteHost);
        if (!m_impl->cert_path.has_value())   missing.emplace_back(kCertPath);
        if (!m_impl->key_path.has_value())    missing.emplace_back(kKeyPath);
        if (!m_impl->ca_path.has_value())     missing.emplace_back(kCaPath);
    }
    if (missing.empty()) return std::nullopt;
    return missing;
}

// ─────────────────────── Write side ─────────────────────────────────

void DsBridge::set_state(const std::string& s) {
    if (!m_ok) return;
    auto rs = m_impl->client.set(kState, s);
    if (!rs.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l set %C='%C' failed: %C\n"),
                   kState, s.c_str(), rs.err.c_str()));
    }
    // Stamp the tunnel connect time exactly on the transition INTO "connected"
    // (so the UI can show uptime = now − vpn.connected.unix), and clear it to 0
    // on the way out so a disconnected tunnel reports no uptime. Only fire on a
    // real transition — set_state() is called for every mgmt STATE line, and
    // re-stamping on each "connected" event would reset the uptime each poll.
    if (s != m_prev_state) {
        if (s == "connected") {
            m_impl->client.set(kConnectedUnix,
                static_cast<std::int32_t>(std::time(nullptr)));
        } else if (m_prev_state == "connected") {
            m_impl->client.set(kConnectedUnix, std::int32_t{0});
        }
        m_prev_state = s;
    }
}
void DsBridge::set_assigned_ip(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kAssignedIp, s);
}
void DsBridge::set_assigned_gateway(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kAssignedGateway, s);
}
void DsBridge::set_assigned_netmask(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kAssignedNetmask, s);
}
void DsBridge::set_assigned_dns(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kAssignedDns, s);
}
void DsBridge::set_pid(std::uint32_t p) {
    if (!m_ok) return;
    m_impl->client.set(kPid, p);
}
void DsBridge::set_exit_code(std::int32_t c) {
    if (!m_ok) return;
    m_impl->client.set(kExitCode, c);
}
void DsBridge::set_gate_reason(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kGateReason, s);
}
void DsBridge::set_bound_iface(const std::string& s) {
    if (!m_ok) return;
    m_impl->client.set(kBoundIface, s);
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

void DsBridge::on_wan_change(WanCallback cb) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    m_impl->wan_cb = std::move(cb);
}

} // namespace openvpn_client
