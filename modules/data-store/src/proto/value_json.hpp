#ifndef __data_store_proto_value_json_hpp__
#define __data_store_proto_value_json_hpp__

/// Server-internal: JSON ↔ Value conversion. Lives under src/proto
/// (not in the public inc/ tree) because it pulls in nlohmann::json,
/// which the client library deliberately avoids in its public ABI.
///
/// The client lib reaches into this header only from its .cpp, never
/// from inc/data_store/client.hpp.

#include "data_store/value.hpp"

#include <limits>
#include <type_traits>

#include "nlohmann/json.hpp"

namespace data_store {

inline Value value_from_json(const nlohmann::json& j) {
    using nlohmann::json;
    if (j.is_null())    return std::monostate{};
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_string())  return j.get<std::string>();
    if (j.is_number_float()) return j.get<double>();
    if (j.is_number_unsigned()) {
        auto u = j.get<std::uint64_t>();
        if (u <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(u);
        }
        // Wider than uint32 → spill to double (we keep the variant
        // grace-shaped; no int64 alternative).
        return static_cast<double>(u);
    }
    if (j.is_number_integer()) {
        auto i = j.get<std::int64_t>();
        if (i >= std::numeric_limits<std::int32_t>::min() &&
            i <= std::numeric_limits<std::int32_t>::max()) {
            return static_cast<std::int32_t>(i);
        }
        return static_cast<double>(i);
    }
    // Arrays / objects → not a scalar value; collapse to monostate.
    // Callers should reject these before reaching here.
    return std::monostate{};
}

inline nlohmann::json value_to_json(const Value& v) {
    using nlohmann::json;
    return std::visit([](auto&& arg) -> json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else return arg;
    }, v);
}

} // namespace data_store

#endif /* __data_store_proto_value_json_hpp__ */
