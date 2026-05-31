#include "ds_config.hpp"

#include <utility>
#include <variant>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace iot {

const char* DsConfig::kDefaultSocketPath = data_store::proto::kDefaultSocketPath;

struct DsConfig::Impl {
    data_store::Client client;
};

namespace {

/// Look up `key` and run the supplied projector against the typed
/// Value. Returns nullopt when the key is unset, the request fails,
/// or the projector rejects the variant alternative.
template <typename T, typename Project>
std::optional<T> fetch(data_store::Client& cli,
                       const std::string&  key,
                       Project             project) {
    std::vector<data_store::Client::GetResult> got;
    auto rs = cli.get({key}, got, /*timeout_ms=*/500);
    if (!rs.ok || got.empty() || !got[0].has_value) return std::nullopt;
    return project(got[0].value);
}

} // namespace

DsConfig::DsConfig(std::string socketPath)
  : m_impl(std::make_unique<Impl>()),
    m_path(socketPath.empty() ? std::string(kDefaultSocketPath)
                              : std::move(socketPath)) {
    auto cs = m_impl->client.connect(m_path);
    if (!cs.ok) {
        // ds-server is optional — log once then move on. The caller
        // checks connected() / each accessor returns nullopt → fallback
        // path runs.
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [iot:%t] %M %N:%l data-store not available "
                            "at %C (%C); using fallback defaults\n"),
                   m_path.c_str(), cs.err.c_str()));
        return;
    }
    // EMP has no welcome handshake — the connection is usable as soon
    // as connect() returns ok.
    m_ok = true;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [iot:%t] %M %N:%l data-store ready at %C\n"),
               m_path.c_str()));
}

DsConfig::~DsConfig() {
    if (m_impl) m_impl->client.close();
}

std::optional<std::string> DsConfig::endpoint() {
    if (!m_ok) return std::nullopt;
    return fetch<std::string>(m_impl->client, "iot.endpoint",
        [](const data_store::Value& v) -> std::optional<std::string> {
            if (std::holds_alternative<std::string>(v)) {
                return std::get<std::string>(v);
            }
            return std::nullopt;
        });
}

std::optional<std::string> DsConfig::server_uri() {
    if (!m_ok) return std::nullopt;
    return fetch<std::string>(m_impl->client, "iot.server.uri",
        [](const data_store::Value& v) -> std::optional<std::string> {
            if (std::holds_alternative<std::string>(v)) {
                return std::get<std::string>(v);
            }
            return std::nullopt;
        });
}

std::optional<std::uint32_t> DsConfig::lifetime() {
    if (!m_ok) return std::nullopt;
    return fetch<std::uint32_t>(m_impl->client, "iot.lifetime",
        [](const data_store::Value& v) -> std::optional<std::uint32_t> {
            if (std::holds_alternative<std::uint32_t>(v)) {
                return std::get<std::uint32_t>(v);
            }
            // Accept int32 (operator may have typed -1 or similar) only
            // when non-negative; reject doubles + strings to keep the
            // schema strict.
            if (std::holds_alternative<std::int32_t>(v)) {
                auto n = std::get<std::int32_t>(v);
                if (n >= 0) return static_cast<std::uint32_t>(n);
            }
            return std::nullopt;
        });
}

} // namespace iot
