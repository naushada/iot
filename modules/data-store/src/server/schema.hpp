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
#include <unordered_set>
#include <vector>

#include "data_store/value.hpp"

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
    std::optional<Value>        default_value;
    std::optional<long long>    min_int;
    std::optional<long long>    max_int;
    std::vector<std::string>    depends_on;  ///< L17a: service names this key depends on
    std::vector<std::string>    write_acl;   ///< L17c: "uid:N" or "gid:name" allowed to write
    std::vector<std::string>    read_acl;    ///< L17c: "uid:N" or "gid:name" allowed to read
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
                                            const Value& value) const;

    /// Default for an absent key, or nullopt when the schema has
    /// no default (or no entry at all).
    std::optional<Value> default_for(const std::string& key) const;

    /// Is the first dot-segment of `key` claimed by some loaded
    /// schema file's `namespace="..."` declaration? Used by
    /// validate_set to reject sets on undeclared keys when their
    /// namespace IS owned (L16/D2 — supports the
    /// "services.ds.enable is intentionally absent" pattern).
    bool is_namespace_claimed(const std::string& key) const;

    /// L17c — check whether a peer with the given uid/gid is
    /// allowed to write `key`. Returns an empty optional on success;
    /// a diagnostic string on rejection. An empty ACL (undeclared)
    /// returns success — the key is unrestricted.
    std::optional<std::string> check_write_acl(
        const std::string& key,
        std::uint32_t uid, std::uint32_t gid) const;

    /// PSK provisioning (task C) — read counterpart of check_write_acl.
    /// Enforces `read_acl` on the Get / RegisterWatch paths so write-only
    /// keys (PSK secrets) cannot be read back by an unauthorised peer
    /// (e.g. ds-cli as root). Same semantics: empty optional = allowed,
    /// diagnostic string = denied, undeclared/empty ACL = unrestricted.
    /// dev-mode bypass is handled by the caller (worker), not here, so
    /// the registry stays pure.
    std::optional<std::string> check_read_acl(
        const std::string& key,
        std::uint32_t uid, std::uint32_t gid) const;

    std::size_t size() const { return m_entries.size(); }
    std::size_t namespace_count() const { return m_namespaces.size(); }

    /// L16/D7 — JSON shape returned by the `schema-dump` protocol
    /// op. Returns nlohmann::json by output-parameter so the header
    /// stays free of <nlohmann/json.hpp> (it isn't strictly public);
    /// declared as a free function in the cpp instead would also
    /// work but bundling on the class keeps discovery simple.
    /// Shape:
    ///   {
    ///     "namespaces": [ "iot", "vpn", ... ],
    ///     "keys": {
    ///       "iot.lifetime": {
    ///         "type":     "integer",
    ///         "default":  86400,
    ///         "min":      0,
    ///         "max":      2592000
    ///       },
    ///       ...
    ///     }
    ///   }
    /// Defined in schema.cpp (the dump uses nlohmann::json under
    /// the hood; ds-cli depends on nlohmann::json transitively
    /// already).
    std::string dump_json() const;

private:
    /// Parse one schema file. Throws std::runtime_error on
    /// fundamental problems; caller catches + skips.
    void load_one(const std::string& path);

    /// Pull the first dot-segment out of `key` (e.g. "services.ds.enable"
    /// -> "services"). Empty when the key has no dots.
    static std::string first_segment(const std::string& key);

    std::unordered_map<std::string, SchemaEntry> m_entries;
    std::unordered_set<std::string>              m_namespaces;
};

} // namespace data_store::server

#endif /* __data_store_server_schema_hpp__ */
