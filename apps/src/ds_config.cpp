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
constexpr const char* kKeyLifetime  = "iot.lifetime";

} // namespace

struct DsConfig::Impl {
    data_store::Client                  client;
    data_store::Client::WatchHandle     watch_handle =
        data_store::Client::kInvalidHandle;

    mutable std::mutex                  mtx;
    std::optional<std::string>          endpoint;
    std::optional<std::string>          server_uri;
    std::optional<std::uint32_t>        lifetime;
    ChangeCallback                      cb;
};

DsConfig::DsConfig(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [iot:%t] %M %N:%l data-store not available "
                            "at %C (%C); using fallback defaults\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [iot:%t] %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));

    // Prime the cache with one initial get for all three keys. Missing
    // values stay nullopt and the caller's value_or() defaults apply.
    std::vector<data_store::Client::GetResult> got;
    auto gs = m_impl->client.get(
        {kKeyEndpoint, kKeyServerUri, kKeyLifetime}, got);
    if (gs.ok) {
        std::lock_guard<std::mutex> g(m_impl->mtx);
        for (auto& r : got) {
            if (!r.has_value) continue;
            if (r.key == kKeyEndpoint) {
                m_impl->endpoint = data_store::to_string(r.value);
            } else if (r.key == kKeyServerUri) {
                m_impl->server_uri = data_store::to_string(r.value);
            } else if (r.key == kKeyLifetime) {
                m_impl->lifetime = data_store::to_uint32(r.value);
            }
        }
    }

    // Watch the same three keys. The callback fires on the Client's
    // listener thread; it updates the cache + invokes the user-side
    // change callback so a long-lived process can react.
    auto ws = m_impl->client.watch(
        {kKeyEndpoint, kKeyServerUri, kKeyLifetime},
        [this](const data_store::Client::Event& ev) {
            std::optional<Key> changed;
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                if (ev.key == kKeyEndpoint) {
                    m_impl->endpoint = data_store::to_string(ev.value);
                    changed = Key::Endpoint;
                } else if (ev.key == kKeyServerUri) {
                    m_impl->server_uri = data_store::to_string(ev.value);
                    changed = Key::ServerUri;
                } else if (ev.key == kKeyLifetime) {
                    m_impl->lifetime = data_store::to_uint32(ev.value);
                    changed = Key::Lifetime;
                }
            }
            if (!changed) return;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [iot:%t] %M %N:%l data-store key '%C' "
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
                   ACE_TEXT("%D [iot:%t] %M %N:%l watch register failed: %C — "
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

std::optional<std::uint32_t> DsConfig::lifetime() const {
    if (!m_ok) return std::nullopt;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    return m_impl->lifetime;
}

void DsConfig::on_change(ChangeCallback cb) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->mtx);
    m_impl->cb = std::move(cb);
}

} // namespace iot
