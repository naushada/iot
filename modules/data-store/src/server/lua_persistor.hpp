#ifndef __data_store_server_lua_persistor_hpp__
#define __data_store_server_lua_persistor_hpp__

/// Loads + flushes the persistent key-value store as a Lua chunk.
///
/// On-disk shape per design.md §4.1:
///
///   return {
///     schema_version = 1,
///     data = {
///       ["foo"] = "bar",
///       ["counter"] = "42",
///       ...
///     },
///   }
///
/// Mirrors the `apps/config/**/*.lua` convention so the same loader
/// (`iot::lua_config`) can be pointed at the file for inspection,
/// though the data-store module ships its own minimal loader instead
/// of cross-linking apps/.
///
/// load() runs once on daemon startup; save() runs after every
/// successful set/remove that changes state (write-through per
/// DS-D2). save() is crash-safe via temp + fsync + rename.

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace data_store::server {

/// Distinguishes "file simply isn't there yet" (recoverable: start
/// with empty map) from "file exists but we can't parse it"
/// (REQ-DS-016: must log + exit 3, not silently start with stale
/// state).
class CorruptStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LuaPersistor {
public:
    explicit LuaPersistor(std::string path);

    /// Read the file. Returns an empty map if it doesn't exist.
    /// Throws CorruptStoreError on parse / schema problems.
    std::unordered_map<std::string, std::string> load();

    /// Atomic write: serialise → write to `<path>.tmp` → fsync →
    /// rename to `<path>`. Throws std::runtime_error on disk error.
    void save(const std::unordered_map<std::string, std::string>& data);

    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

} // namespace data_store::server

#endif /* __data_store_server_lua_persistor_hpp__ */
