#ifndef __data_store_server_data_store_hpp__
#define __data_store_server_data_store_hpp__

/// Server-internal in-memory key-value store + per-key watch table.
///
/// Values are typed via `data_store::Value` (variant of null /
/// string / bool / uint32 / int32 / double — grace-server shape).
/// Shared across all Worker threads, so every public method takes
/// the internal mutex.

#include <chrono>
#include <cstddef>
#include <cstdint>
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

    /// L17d — rate-limit window in milliseconds. 0 = disabled.
    /// When non-zero, any set()/set_volatile() within this window
    /// of the last set on the same key returns RateLimited.
    void set_rate_limit_ms(std::uint32_t ms) { m_rate_limit_ms = ms; }
    std::uint32_t rate_limit_ms() const { return m_rate_limit_ms; }

    /// Bulk replace — called at startup after LuaPersistor::load().
    void load_from(std::unordered_map<std::string, Value> data);

    std::size_t size() const;

    /// Read. Returns nullopt for missing keys.
    std::optional<Value> get(const std::string& key) const;

    /// Write. Returns SetResult describing the change + watchers.
    /// If a volatile entry exists for this key, it is cleared
    /// (persistent write takes precedence).
    SetResult set(const std::string& key, Value value);

    /// Volatile write (L17b). Writes to an in-memory overlay that
    /// is NOT flushed to the persistor. Survives until the server
    /// restarts OR a persistent `set()` clears it. Fires the same
    /// change notifications as a normal set so watchers see the
    /// transition.
    SetResult set_volatile(const std::string& key, Value value);

    /// L17d — check whether `key` was set within the rate-limit
    /// window. Returns true when the set should be rejected.
    /// When the window is 0, always returns false.
    bool is_rate_limited(const std::string& key);

    /// Remove. Returns true if the key existed.
    bool remove(const std::string& key);

    // ---- Watch table ----------------------------------------------------

    void watch(Session* s, const std::string& key);
    void unwatch(Session* s, const std::string& key);
    void unwatch_all(Session* s);

private:
    void flush_locked_release(
        std::unordered_map<std::string, Value> snapshot,
        std::uint64_t                          gen);

    mutable std::mutex                                              m_mtx;
    // Persist serialisation: worker threads flush with m_mtx released,
    // so concurrent saves must be ordered — they otherwise truncate +
    // rename the same .tmp file (rename → ENOENT) and an older
    // snapshot can land on disk last. m_snap_gen is bumped under m_mtx
    // when a snapshot is taken; m_flushed_gen (under m_flush_mtx)
    // tracks the newest generation persisted.
    std::mutex                                                      m_flush_mtx;
    std::uint64_t                                                   m_snap_gen = 0;
    std::uint64_t                                                   m_flushed_gen = 0;
    std::unordered_map<std::string, Value>                          m_data;
    std::unordered_map<std::string, Value>                          m_volatile;  // L17b: in-memory overlay
    std::unordered_map<std::string, std::unordered_set<Session*>>   m_watchers;
    LuaPersistor*                                                   m_persistor = nullptr;
    std::uint32_t                                                   m_rate_limit_ms = 0;  // L17d: 0=off
    std::unordered_map<std::string,
        std::chrono::steady_clock::time_point>                      m_last_set;  // L17d: per-key last-set time
};

} // namespace data_store::server

#endif /* __data_store_server_data_store_hpp__ */
