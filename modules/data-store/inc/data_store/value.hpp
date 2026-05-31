#ifndef __data_store_value_hpp__
#define __data_store_value_hpp__

/// Typed values carried by the data-store protocol.
///
/// Same variant shape as grace-server's lua_engine value_type
/// (modules/data-store/docs/design.md §3 + DS-D11). The wire
/// protocol now carries JSON values directly (no string coercion),
/// the in-memory store keys onto a `Value`, and the Lua persistor
/// writes the matching Lua-native type to disk.
///
/// `std::monostate` rather than `std::nullptr_t` is used for the
/// "absent" / "null" alternative so the variant is default-
/// constructible and `std::get<std::monostate>` reads cleanly.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace data_store {

using Value = std::variant<
    std::monostate,     // JSON null
    std::string,
    bool,
    std::uint32_t,
    std::int32_t,
    double>;

/// Convenience kind enum for readable dispatch on Value::index().
enum class ValueKind : std::uint8_t {
    Null    = 0,
    String  = 1,
    Boolean = 2,
    Uint    = 3,
    Int     = 4,
    Double  = 5,
};

inline ValueKind kind_of(const Value& v) {
    return static_cast<ValueKind>(v.index());
}

// ────────────────────── Typed accessors ───────────────────────────────
// Coerce a Value into a specific alternative. Strict by default — type
// mismatch returns nullopt — with two pragmatic numeric promotions:
//
//   to_uint32 accepts an int32 ≥ 0     (schema's min=0 catches negatives
//                                        at set time; this is defensive
//                                        for keys without that gate)
//   to_int32  accepts a uint32 that fits in INT32_MAX
//   to_double accepts uint32 / int32   (every integer fits in a double)
//
// String/bool alternatives are NOT coerced from anything else — callers
// who want "1" → true or "42" → uint32 do their own parsing.
//
// These helpers exist so apps consuming libdatastore_client don't have
// to re-roll the std::get_if + promotion boilerplate per field. The
// iot binary (apps/src/ds_config.cpp) and openvpn-client
// (modules/openvpn/client/src/ds_bridge.cpp) both use these.

inline std::optional<std::string> to_string(const Value& v) {
    if (auto* p = std::get_if<std::string>(&v)) return *p;
    return std::nullopt;
}

inline std::optional<bool> to_bool(const Value& v) {
    if (auto* p = std::get_if<bool>(&v)) return *p;
    return std::nullopt;
}

inline std::optional<std::uint32_t> to_uint32(const Value& v) {
    if (auto* p = std::get_if<std::uint32_t>(&v)) return *p;
    if (auto* p = std::get_if<std::int32_t>(&v); p && *p >= 0) {
        return static_cast<std::uint32_t>(*p);
    }
    return std::nullopt;
}

inline std::optional<std::int32_t> to_int32(const Value& v) {
    if (auto* p = std::get_if<std::int32_t>(&v)) return *p;
    if (auto* p = std::get_if<std::uint32_t>(&v);
            p && *p <= static_cast<std::uint32_t>(
                            std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(*p);
    }
    return std::nullopt;
}

inline std::optional<double> to_double(const Value& v) {
    if (auto* p = std::get_if<double>(&v))        return *p;
    if (auto* p = std::get_if<std::uint32_t>(&v)) return static_cast<double>(*p);
    if (auto* p = std::get_if<std::int32_t>(&v))  return static_cast<double>(*p);
    return std::nullopt;
}

} // namespace data_store

#endif /* __data_store_value_hpp__ */
