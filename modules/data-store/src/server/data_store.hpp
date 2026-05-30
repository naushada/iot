#ifndef __data_store_server_data_store_hpp__
#define __data_store_server_data_store_hpp__

/// Server-internal: in-memory key-value store. Single-threaded —
/// owned exclusively by the ds-server reactor thread. D1 lands the
/// type + empty map; D3 wires it through the unix-socket dispatcher;
/// D4 layers Lua persistence on top.

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace data_store::server {

class DataStore {
public:
    DataStore() = default;
    DataStore(const DataStore&)            = delete;
    DataStore& operator=(const DataStore&) = delete;

    std::size_t size() const { return m_data.size(); }
    std::optional<std::string> get(const std::string& key) const;
    std::optional<std::string> set(const std::string& key,
                                   const std::string& value);
    bool remove(const std::string& key);

private:
    std::unordered_map<std::string, std::string> m_data;
};

} // namespace data_store::server

#endif /* __data_store_server_data_store_hpp__ */
