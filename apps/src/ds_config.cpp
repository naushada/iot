#include "ds_config.hpp"

#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace iot {

const char* DsConfig::kDefaultSocketPath = data_store::proto::kDefaultSocketPath;

namespace {

constexpr const char* kKeyEndpoint  = "iot.endpoint";
constexpr const char* kKeyServerUri = "iot.server.uri";
constexpr const char* kKeyBsUri     = "iot.bs.uri";
constexpr const char* kKeyLifetime  = "iot.lifetime";
// PSK provisioning keys (tasks E/F/G).
constexpr const char* kKeySerial      = "iot.serial";
constexpr const char* kKeyBsPskId     = "iot.bs.psk.identity";
constexpr const char* kKeyBsPskKey    = "iot.bs.psk.key";
constexpr const char* kKeyDmPskId     = "iot.dm.psk.identity";
constexpr const char* kKeyDmPskKey    = "iot.dm.psk.key";
constexpr const char* kKeyDevMode     = "iot.dev.mode";
// LwM2M connection lifecycle published to the device-ui (write-only here).
constexpr const char* kKeyConnState   = "iot.conn.state";

} // namespace

struct DsConfig::Impl {
    data_store::Client                  client;
    data_store::Client::WatchHandle     watch_handle =
        data_store::Client::kInvalidHandle;

    mutable std::mutex                  mtx;
    std::optional<std::string>          endpoint;
    std::optional<std::string>          server_uri;
    std::optional<std::string>          bs_uri;
    std::optional<std::uint32_t>        lifetime;
    // PSK provisioning cache (tasks E/F/G).
    std::optional<std::string>          serial;
    std::optional<std::string>          bs_psk_identity;
    std::optional<std::string>          bs_psk_key;
    std::optional<std::string>          dm_psk_identity;
    std::optional<std::string>          dm_psk_key;
    bool                                dev_mode = false;
    ChangeCallback                      cb;

    /// Update the cache for one key/value. Caller holds `mtx`. Returns
    /// the matching DsConfig::Key (for change notification) or nullopt
    /// when the key isn't one we track.
    std::optional<Key> apply(const std::string& key,
                             const data_store::Value& v) {
        if (key == kKeyEndpoint)   { endpoint = data_store::to_string(v); return Key::Endpoint; }
        if (key == kKeyServerUri)  { server_uri = data_store::to_string(v); return Key::ServerUri; }
        if (key == kKeyBsUri)      { bs_uri = data_store::to_string(v); return Key::BsUri; }
        if (key == kKeyLifetime)   { lifetime = data_store::to_uint32(v); return Key::Lifetime; }
        if (key == kKeySerial)     { serial = data_store::to_string(v); return Key::Serial; }
        if (key == kKeyBsPskId)    { bs_psk_identity = data_store::to_string(v); return Key::BsPskIdentity; }
        if (key == kKeyBsPskKey)   { bs_psk_key = data_store::to_string(v); return Key::BsPskKey; }
        if (key == kKeyDmPskId)    { dm_psk_identity = data_store::to_string(v); return Key::DmPskIdentity; }
        if (key == kKeyDmPskKey)   { dm_psk_key = data_store::to_string(v); return Key::DmPskKey; }
        if (key == kKeyDevMode)    {
            if (auto b = data_store::to_bool(v)) dev_mode = *b;
            return Key::DevMode;
        }
        return std::nullopt;
    }
};

DsConfig::DsConfig(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l data-store not available "
                            "at %C (%C); using fallback defaults\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));

    // All keys this reader caches + watches.
    const std::vector<std::string> kKeys = {
        kKeyEndpoint, kKeyServerUri, kKeyBsUri, kKeyLifetime,
        kKeySerial, kKeyBsPskId, kKeyBsPskKey,
        kKeyDmPskId, kKeyDmPskKey, kKeyDevMode,
    };

    // Prime the cache with one initial get for all keys. Missing values
    // stay nullopt and the caller's value_or() defaults apply.
    std::vector<data_store::Client::GetResult> got;
    auto gs = m_impl->client.get(kKeys, got);
    if (gs.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        for (auto& r : got) {
            if (!r.has_value) continue;
            m_impl->apply(r.key, r.value);
        }
    }

    // Watch the same keys. The callback fires on the Client's listener
    // thread; it updates the cache + invokes the user-side change
    // callback so a long-lived process can react.
    auto ws = m_impl->client.watch(
        kKeys,
        [this](const data_store::Client::Event& ev) {
            std::optional<Key> changed;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                changed = m_impl->apply(ev.key, ev.value);
            }
            if (!changed) return;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l data-store key '%C' "
                                "changed — registered on_change handlers fire "
                                "next\n"),
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
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l watch register failed: %C — "
                            "cache primed but hot-reload disabled\n"),
                   ws.err.c_str()));
    }
}

DsConfig::~DsConfig() {
    if (!m_impl) return;
    if (m_impl->watch_handle != data_store::Client::kInvalidHandle) {
        m_impl->client.unwatch(m_impl->watch_handle, /*timeout_ms=*/200);
    }
    m_impl->client.close();
}

data_store::Client* DsConfig::client() {
    if (!m_ok || !m_impl) return nullptr;
    return &m_impl->client;
}

std::optional<std::string> DsConfig::endpoint() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->endpoint;
}

std::optional<std::string> DsConfig::server_uri() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->server_uri;
}

std::optional<std::string> DsConfig::bs_uri() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->bs_uri;
}

std::optional<std::uint32_t> DsConfig::lifetime() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->lifetime;
}

std::optional<std::string> DsConfig::serial() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->serial;
}

std::optional<std::string> DsConfig::bs_psk_identity() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->bs_psk_identity;
}

std::optional<std::string> DsConfig::bs_psk_key() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->bs_psk_key;
}

std::optional<std::string> DsConfig::dm_psk_identity() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dm_psk_identity;
}

std::optional<std::string> DsConfig::dm_psk_key() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dm_psk_key;
}

bool DsConfig::dev_mode() const {
    if (!m_ok) return false;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->dev_mode;
}

bool DsConfig::set_serial(const std::string& serial) {
    if (!m_ok || !m_impl) return false;
    // RPi auto-fill: the serial IS the endpoint and the BS PSK identity.
    auto s = m_impl->client.set({
        {kKeySerial,   data_store::Value{serial}},
        {kKeyEndpoint, data_store::Value{serial}},
        {kKeyBsPskId,  data_store::Value{serial}},
    });
    // Reflect locally so a subsequent self-write watch event is absorbed.
    if (s.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        m_impl->serial = serial;
        m_impl->endpoint = serial;
        m_impl->bs_psk_identity = serial;
    }
    return s.ok;
}

bool DsConfig::set_dm_credentials(const std::string& identity,
                                  const std::string& key_hex) {
    if (!m_ok || !m_impl) return false;
    auto s = m_impl->client.set({
        {kKeyDmPskId,  data_store::Value{identity}},
        {kKeyDmPskKey, data_store::Value{key_hex}},
    });
    if (s.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        m_impl->dm_psk_identity = identity;
        m_impl->dm_psk_key = key_hex;
    }
    return s.ok;
}

bool DsConfig::set_conn_state(const std::string& state) {
    if (!m_ok || !m_impl) return false;
    // iot.conn.state is published, never read back into the cache — the
    // client is the sole writer, so there's nothing to mirror locally.
    auto s = m_impl->client.set({
        {kKeyConnState, data_store::Value{state}},
    });
    return s.ok;
}

void DsConfig::on_change(ChangeCallback cb) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    m_impl->cb = std::move(cb);
}

} // namespace iot
