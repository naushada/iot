#ifndef __data_store_server_data_store_hpp__
#define __data_store_server_data_store_hpp__

/// Server-internal in-memory key-value store + per-key watch table.
///
/// Shared across all Worker threads, so every public method takes
/// the internal mutex. Workers call into this when servicing a
/// session request; the reactor thread never touches it.
///
/// The watch table maps `key → set of Session*`. `set()` returns the
/// snapshot of watchers so the caller can dispatch DeliverNotify
/// messages to each watcher's owning Worker without holding the
/// mutex across socket writes.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace data_store::server {

class Session;   // fwd; defined in session.hpp

/// Result of a `set` call. `changed` is true iff the new value
/// differed from the prior one (or the key was absent). `prev` is
/// nullopt when the key was new. `watchers` is the snapshot of
/// Sessions subscribed to this key at write time — safe to use
/// after the store mutex is released.
struct SetResult {
    bool                      changed = false;
    std::optional<std::string> prev;
    std::vector<Session*>     watchers;
};

class DataStore {
public:
    DataStore() = default;
    DataStore(const DataStore&)            = delete;
    DataStore& operator=(const DataStore&) = delete;

    std::size_t size() const;

    /// Read. Returns nullopt for missing keys.
    std::optional<std::string> get(const std::string& key) const;

    /// Write. Returns SetResult describing the change + watchers.
    SetResult set(const std::string& key, const std::string& value);

    /// Remove. Returns true if the key existed. No notifications
    /// fired today per DS-D5 (reserved for v2).
    bool remove(const std::string& key);

    // ---- Watch table ----------------------------------------------------

    void watch(Session* s, const std::string& key);
    void unwatch(Session* s, const std::string& key);
    void unwatch_all(Session* s);

private:
    mutable std::mutex                                              m_mtx;
    std::unordered_map<std::string, std::string>                    m_data;
    std::unordered_map<std::string, std::unordered_set<Session*>>   m_watchers;
};

} // namespace data_store::server

#endif /* __data_store_server_data_store_hpp__ */
