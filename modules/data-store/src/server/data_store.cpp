#include "data_store.hpp"

#include "lua_persistor.hpp"

#include <ace/Log_Msg.h>

namespace data_store::server {

void DataStore::load_from(std::unordered_map<std::string, Value> data) {
    std::lock_guard<std::mutex> g(m_mtx);
    m_data = std::move(data);
}

void DataStore::flush_locked_release(
        std::unordered_map<std::string, Value> snapshot) {
    if (!m_persistor) return;
    try {
        m_persistor->save(snapshot);
    } catch (const std::exception& e) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [DS:%t] %M %N:%l persist failed: %C\n"),
                   e.what()));
    }
}

std::size_t DataStore::size() const {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_data.size();
}

std::optional<Value> DataStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> g(m_mtx);
    auto it = m_data.find(key);
    if (it == m_data.end()) return std::nullopt;
    return it->second;
}

SetResult DataStore::set(const std::string& key, Value value) {
    SetResult out;
    std::unordered_map<std::string, Value> snapshot;
    {
        std::lock_guard<std::mutex> g(m_mtx);

        auto it = m_data.find(key);
        if (it == m_data.end()) {
            m_data.emplace(key, std::move(value));
            out.changed = true;
        } else {
            if (it->second == value) {
                // REQ-DS-006: unchanged value → no notification + no flush.
                return out;
            }
            out.prev    = std::move(it->second);
            out.changed = true;
            it->second  = std::move(value);
        }

        auto wit = m_watchers.find(key);
        if (wit != m_watchers.end()) {
            out.watchers.reserve(wit->second.size());
            for (Session* s : wit->second) out.watchers.push_back(s);
        }

        if (m_persistor) snapshot = m_data;
    }
    flush_locked_release(std::move(snapshot));
    return out;
}

bool DataStore::remove(const std::string& key) {
    bool existed = false;
    std::unordered_map<std::string, Value> snapshot;
    {
        std::lock_guard<std::mutex> g(m_mtx);
        existed = m_data.erase(key) > 0;
        if (existed && m_persistor) snapshot = m_data;
    }
    if (existed) flush_locked_release(std::move(snapshot));
    return existed;
}

void DataStore::watch(Session* s, const std::string& key) {
    if (!s) return;
    std::lock_guard<std::mutex> g(m_mtx);
    m_watchers[key].insert(s);
}

void DataStore::unwatch(Session* s, const std::string& key) {
    if (!s) return;
    std::lock_guard<std::mutex> g(m_mtx);
    auto it = m_watchers.find(key);
    if (it == m_watchers.end()) return;
    it->second.erase(s);
    if (it->second.empty()) m_watchers.erase(it);
}

void DataStore::unwatch_all(Session* s) {
    if (!s) return;
    std::lock_guard<std::mutex> g(m_mtx);
    for (auto it = m_watchers.begin(); it != m_watchers.end(); ) {
        it->second.erase(s);
        if (it->second.empty()) it = m_watchers.erase(it);
        else                    ++it;
    }
}

} // namespace data_store::server
