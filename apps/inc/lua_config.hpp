#ifndef __lua_config_hpp__
#define __lua_config_hpp__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

/// Tiny Lua loader for apps/config/{deviceObject,serverObject,securityObject}/*.lua.
///
/// Every config file ships the shape
///
///   return {
///     <object_name> = {
///       instance  = <iid>,
///       resources = {
///         [<rid>] = { description = "...", value = ..., include = true|false },
///         ...
///       },
///     },
///   }
///
/// This loader walks that shape and returns a flat `rid → ResourceRecord`
/// map. `description` is preserved for debug logging; only `value` and
/// `include` are normally consumed by the upstream loaders in
/// lwm2m_object_3_device.cpp and main.cpp.
///
/// Value vocabulary (matches what the legacy JSON loaders read):
///   - bool         → boolean
///   - long long    → integer    (Lua numbers without fraction part)
///   - double       → number     (Lua numbers with fraction part)
///   - std::string  → string
///   - std::vector<std::uint8_t> → opaque   (Lua sub-table
///                                           `{ bytes = {...}, subtype = N }`)
///   - std::monostate → unset / unrecognised  (caller falls back to default)
///
/// On missing file / parse error / unexpected shape the loader returns an
/// empty map and logs a single warning line. The caller's existing
/// fallback-to-default behaviour then takes over.

namespace iot::lua_config {

using OpaqueBytes = std::vector<std::uint8_t>;

using ResourceValue =
    std::variant<std::monostate, bool, long long, double, std::string, OpaqueBytes>;

struct ResourceRecord {
    std::string   description;
    ResourceValue value;
    bool          include = false;
};

using ResourceMap = std::unordered_map<std::uint32_t, ResourceRecord>;

/// Load the .lua file at `path`. Returns an empty map on any error
/// (file missing, syntax error, unexpected shape). The caller decides
/// whether an empty map is fatal.
ResourceMap load_object_resources(const std::string& path);

// ----- typed accessors (used by lwm2m_object_stubs.cpp object installers) -----

/// Read the value of the entry at `rid` as a string. Returns `def`
/// when the rid is absent, the entry's value isn't a string, or the
/// entry is excluded by `include = false`.
std::string string_or(const ResourceMap& m, std::uint32_t rid, const std::string& def);

/// Read the value of the entry at `rid` as an unsigned integer. Same
/// fallback rules.
std::uint32_t uint_or(const ResourceMap& m, std::uint32_t rid, std::uint32_t def);

/// Read the value of the entry at `rid` as a bool. Same fallback rules.
bool bool_or(const ResourceMap& m, std::uint32_t rid, bool def);

} // namespace iot::lua_config

#endif /* __lua_config_hpp__ */
