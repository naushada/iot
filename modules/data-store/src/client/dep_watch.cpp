#include "data_store/dep_watch.hpp"

#include <utility>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/value.hpp"

namespace data_store {

std::string DepWatch::make_state_key(const std::string& name) {
    return "services." + name + ".state";
}

bool DepWatch::is_healthy_state(std::string_view s) {
    return s == "running" || s == "starting";
}

DepWatch::DepWatch(Client& client, std::vector<std::string> deps)
    : m_client(client), m_deps(std::move(deps)) {
    m_state_keys.reserve(m_deps.size());
    m_healthy.resize(m_deps.size(), true);   // absent key → schema default "running"
    m_watch_handles.reserve(m_deps.size());

    for (std::size_t i = 0; i < m_deps.size(); ++i) {
        auto state_key = make_state_key(m_deps[i]);
        m_state_keys.push_back(state_key);

        // Prime: get the current state value.
        std::vector<Client::GetResult> got;
        auto gs = m_client.get(std::vector<std::string>{state_key}, got);
        if (gs.ok && !got.empty() && got[0].has_value) {
            if (auto s = to_string(got[0].value); s.has_value()) {
                std::lock_guard<std::mutex> g(m_mtx);
                m_healthy[i] = is_healthy_state(*s);
            }
        } else if (!gs.ok) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [depwatch:%t] %M %N:%l prime %C failed: %C "
                                "(assuming healthy)\n"),
                       state_key.c_str(), gs.err.c_str()));
        }

        // Register a callback watch for state changes.
        Client::WatchHandle handle = Client::kInvalidHandle;
        auto ws = m_client.watch(
            state_key,
            [this, i](const Client::Event& ev) {
                bool changed = false;
                if (auto s = to_string(ev.value); s.has_value()) {
                    bool healthy = is_healthy_state(*s);
                    {
                        std::lock_guard<std::mutex> g(m_mtx);
                        if (m_healthy[i] != healthy) {
                            m_healthy[i] = healthy;
                            ++m_version;
                            changed = true;
                        }
                    }
                }
                if (changed) m_cv.notify_all();
            },
            &handle);
        if (!ws.ok) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [depwatch:%t] %M %N:%l watch %C failed: %C "
                                "(dep state will not update)\n"),
                       state_key.c_str(), ws.err.c_str()));
        }
        m_watch_handles.push_back(handle);
    }
}

DepWatch::~DepWatch() {
    shutdown();
    for (auto h : m_watch_handles) {
        if (h != Client::kInvalidHandle) {
            m_client.unwatch(h, /*timeout_ms=*/200);
        }
    }
    m_watch_handles.clear();
}

bool DepWatch::healthy() const {
    std::lock_guard<std::mutex> g(m_mtx);
    for (bool h : m_healthy) {
        if (!h) return false;
    }
    return true;
}

std::string DepWatch::unhealthy_dep() const {
    std::lock_guard<std::mutex> g(m_mtx);
    for (std::size_t i = 0; i < m_deps.size(); ++i) {
        if (!m_healthy[i]) return m_deps[i];
    }
    return {};
}

bool DepWatch::wait() {
    std::unique_lock<std::mutex> g(m_mtx);
    const auto start_version = m_version;
    m_cv.wait(g, [&] {
        return m_shutdown || m_version != start_version;
    });
    return !m_shutdown;
}

void DepWatch::shutdown() {
    {
        std::lock_guard<std::mutex> g(m_mtx);
        if (m_shutdown) return;
        m_shutdown = true;
    }
    m_cv.notify_all();
}

} // namespace data_store
