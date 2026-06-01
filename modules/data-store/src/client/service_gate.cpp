#include "data_store/service_gate.hpp"

#include <utility>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/value.hpp"

namespace data_store {

namespace {
std::string make_enable_key(const std::string& name) {
    return "services." + name + ".enable";
}
std::string make_state_key(const std::string& name) {
    return "services." + name + ".state";
}
} // namespace

ServiceGate::ServiceGate(Client& client, std::string name)
    : m_client(client),
      m_name(std::move(name)),
      m_enable_key(make_enable_key(m_name)),
      m_state_key(make_state_key(m_name)) {
    // Prime synchronously via get(): the schema default of true
    // populates the snapshot even when the key was never set.
    std::vector<Client::GetResult> got;
    auto gs = m_client.get(std::vector<std::string>{m_enable_key}, got);
    if (gs.ok && !got.empty() && got[0].has_value) {
        if (auto b = to_bool(got[0].value); b.has_value()) {
            std::lock_guard<std::mutex> g(m_mtx);
            m_enabled = *b;
        }
    } else if (!gs.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [svcgate:%t] %M %N:%l prime %C failed: %C "
                            "(falling back to schema default true)\n"),
                   m_enable_key.c_str(), gs.err.c_str()));
    }

    // Register a callback watch. The listener thread updates the
    // snapshot under m_mtx and notifies every waiter on every
    // observed change.
    Client::WatchHandle handle = Client::kInvalidHandle;
    auto ws = m_client.watch(
        m_enable_key,
        [this](const Client::Event& ev) {
            if (auto b = to_bool(ev.value); b.has_value()) {
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_enabled = *b;
                    ++m_version;
                }
                m_cv.notify_all();
            }
        },
        &handle);
    if (!ws.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [svcgate:%t] %M %N:%l watch %C failed: %C "
                            "(gate will not see future changes)\n"),
                   m_enable_key.c_str(), ws.err.c_str()));
    }
    m_watch_handle = handle;
}

ServiceGate::~ServiceGate() {
    shutdown();
    if (m_watch_handle != Client::kInvalidHandle) {
        m_client.unwatch(m_watch_handle, /*timeout_ms=*/200);
        m_watch_handle = Client::kInvalidHandle;
    }
}

bool ServiceGate::enabled() const {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_enabled;
}

std::optional<bool> ServiceGate::wait() {
    std::unique_lock<std::mutex> g(m_mtx);
    const auto start_version = m_version;
    m_cv.wait(g, [&] {
        return m_shutdown || m_version != start_version;
    });
    if (m_shutdown) return std::nullopt;
    return m_enabled;
}

void ServiceGate::shutdown() {
    {
        std::lock_guard<std::mutex> g(m_mtx);
        if (m_shutdown) return;
        m_shutdown = true;
    }
    m_cv.notify_all();
}

void ServiceGate::publish_state(std::string_view s) {
    auto rs = m_client.set(m_state_key, std::string(s));
    if (!rs.ok) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [svcgate:%t] %M %N:%l set %C='%C' failed: %C\n"),
                   m_state_key.c_str(), std::string(s).c_str(),
                   rs.err.c_str()));
    }
}

} // namespace data_store
