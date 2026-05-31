#ifndef __data_store_server_schema_hpp__
#define __data_store_server_schema_hpp__

/// Optional client-published schema for the data store.
///
/// Operators / packagers drop *.lua files under a directory (default
/// `/etc/iot/ds-schemas/`); ds-server scans the directory at startup,
/// indexes every key spec by full key name. Schemas are immutable
/// after load — no locks needed on the read path.
///
/// Each client / app owns one .lua file in the directory; the
/// registry merges every file into a single key→spec map. Two
/// files claiming the same key get a stderr warning (last-loaded
/// wins) so silent ownership collisions don't slip past.
///
/// On-disk shape (one file per client/user):
///
///   return {
///     namespace = "iot",
///     keys = {
///       ["iot.lifetime"]  = { type="integer", default=86400, min=0 },
///       ["iot.endpoint"]  = { type="string",  default="urn:dev:client-1" },
///       ["iot.identity"]  = { type="opaque" },
///     },
///   }
///
/// Validation rules:
///   - set rejects type mismatches (`{ok:false, err:"..."}`)
///   - get returns the schema default when the key is absent
///   - unknown keys (no schema entry) pass through unchanged

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace data_store::server {

enum class SchemaType : std::uint8_t {
    String,
    Integer,
    Float,
    Boolean,
    Opaque,
    Any,        ///< no schema entry / no type field — anything goes
};

struct SchemaEntry {
    SchemaType                  type = SchemaType::Any;
    std::optional<std::string>  default_value;   ///< stringified
    std::optional<long long>    min_int;
    std::optional<long long>    max_int;
};

class SchemaRegistry {
public:
    SchemaRegistry() = default;

    /// Load every *.lua under `dir`. Returns the number of keys
    /// indexed. Missing directory → 0 (not an error). A malformed
    /// individual file is logged to stderr and skipped so one bad
    /// schema doesn't take the daemon down.
    std::size_t load_directory(const std::string& dir);

    /// Lookup a spec for `key`. Returns nullptr when the key isn't
    /// in any loaded schema — the caller treats this as "passthrough".
    const SchemaEntry* find(const std::string& key) const;

    /// Validate a set value against the schema. Returns an empty
    /// optional on success; a diagnostic string on rejection.
    std::optional<std::string> validate_set(const std::string& key,
                                            const std::string& value) const;

    /// Default for an absent key, or nullopt when the schema has
    /// no default (or no entry at all).
    std::optional<std::string> default_for(const std::string& key) const;

    std::size_t size() const { return m_entries.size(); }

private:
    /// Parse one schema file. Throws std::runtime_error on
    /// fundamental problems; caller catches + skips.
    void load_one(const std::string& path);

    std::unordered_map<std::string, SchemaEntry> m_entries;
};

} // namespace data_store::server

#endif /* __data_store_server_schema_hpp__ */
