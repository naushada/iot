#ifndef __data_store_server_lua_persistor_hpp__
#define __data_store_server_lua_persistor_hpp__

/// Loads + flushes the persistent key-value store as a Lua chunk
/// of TYPED values (DS-D11).
///
/// On-disk shape:
///
///   return {
///     schema_version = 2,
///     data = {
///       ["foo"]     = "bar",       -- string
///       ["counter"] = 42,          -- integer
///       ["ratio"]   = 1.5,         -- float
///       ["enabled"] = true,        -- boolean
///       ["unset"]   = nil,         -- absent (serialiser skips nils)
///       -- ...
///     },
///   }
///
/// Each value lands in the right `data_store::Value` variant
/// alternative on load. Schema-version 1 (string-only) files still
/// load — every value comes back as Value::string.

#include <stdexcept>
#include <string>
#include <unordered_map>

#include "data_store/value.hpp"

namespace data_store::server {

class CorruptStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LuaPersistor {
public:
    explicit LuaPersistor(std::string path);

    /// Read the file. Empty map when missing; throws on parse failure.
    std::unordered_map<std::string, Value> load();

    /// Atomic write: serialise → write `<path>.tmp` → fsync → rename.
    void save(const std::unordered_map<std::string, Value>& data);

    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

} // namespace data_store::server

#endif /* __data_store_server_lua_persistor_hpp__ */
