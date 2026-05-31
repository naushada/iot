#ifndef __data_store_server_data_store_hpp__
#define __data_store_server_data_store_hpp__

/// Server-internal in-memory key-value store + per-key watch table.
///
/// Values are typed via `data_store::Value` (variant of null /
/// string / bool / uint32 / int32 / double — grace-server shape).
/// Shared across all Worker threads, so every public method takes
/// the internal mutex.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "data_store/value.hpp"

namespace data_store::server {

class LuaPersistor;
class Session;   // fwd; defined in session.hpp

struct SetResult {
    bool                  changed = false;
    std::optional<Value>  prev;
    std::vector<Session*> watchers;
};

class DataStore {
public:
    DataStore() = default;
    DataStore(const DataStore&)            = delete;
    DataStore& operator=(const DataStore&) = delete;

    /// Attach an (optional) Lua-backed persistor. set() / remove()
    /// flush after every successful state change.
    void set_persistor(LuaPersistor* p) { m_persistor = p; }

    /// Bulk replace — called at startup after LuaPersistor::load().
    void load_from(std::unordered_map<std::string, Value> data);

    std::size_t size() const;

    /// Read. Returns nullopt for missing keys.
    std::optional<Value> get(const std::string& key) const;

    /// Write. Returns SetResult describing the change + watchers.
    SetResult set(const std::string& key, Value value);

    /// Remove. Returns true if the key existed.
    bool remove(const std::string& key);

    // ---- Watch table ----------------------------------------------------

    void watch(Session* s, const std::string& key);
    void unwatch(Session* s, const std::string& key);
    void unwatch_all(Session* s);

private:
    void flush_locked_release(
        std::unordered_map<std::string, Value> snapshot);

    mutable std::mutex                                              m_mtx;
    std::unordered_map<std::string, Value>                          m_data;
    std::unordered_map<std::string, std::unordered_set<Session*>>   m_watchers;
    LuaPersistor*                                                   m_persistor = nullptr;
};

} // namespace data_store::server

#endif /* __data_store_server_data_store_hpp__ */
