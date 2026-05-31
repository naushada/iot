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

} // namespace data_store

#endif /* __data_store_value_hpp__ */
