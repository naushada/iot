#include "data_store.hpp"

namespace data_store::server {

std::size_t DataStore::size() const {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_data.size();
}

std::optional<std::string> DataStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> g(m_mtx);
    auto it = m_data.find(key);
    if (it == m_data.end()) return std::nullopt;
    return it->second;
}

SetResult DataStore::set(const std::string& key, const std::string& value) {
    SetResult out;
    std::lock_guard<std::mutex> g(m_mtx);

    auto it = m_data.find(key);
    if (it == m_data.end()) {
        m_data.emplace(key, value);
        out.changed = true;
    } else {
        if (it->second == value) {
            // REQ-DS-006: unchanged value → no notification.
            return out;
        }
        out.prev    = it->second;
        out.changed = true;
        it->second  = value;
    }

    // Snapshot the watcher set for this key so the caller can
    // dispatch notifications without holding the lock.
    auto wit = m_watchers.find(key);
    if (wit != m_watchers.end()) {
        out.watchers.reserve(wit->second.size());
        for (Session* s : wit->second) out.watchers.push_back(s);
    }
    return out;
}

bool DataStore::remove(const std::string& key) {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_data.erase(key) > 0;
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
