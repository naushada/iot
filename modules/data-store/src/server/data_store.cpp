#include "data_store.hpp"

namespace data_store::server {

std::optional<std::string> DataStore::get(const std::string& key) const {
    auto it = m_data.find(key);
    if (it == m_data.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> DataStore::set(const std::string& key,
                                           const std::string& value) {
    auto it = m_data.find(key);
    std::optional<std::string> prev;
    if (it != m_data.end()) {
        prev = it->second;
        it->second = value;
    } else {
        m_data.emplace(key, value);
    }
    return prev;
}

bool DataStore::remove(const std::string& key) {
    return m_data.erase(key) > 0;
}

} // namespace data_store::server
