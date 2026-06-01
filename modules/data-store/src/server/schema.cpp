#include "schema.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <type_traits>
#include <variant>

#include <ace/Log_Msg.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace data_store::server {

namespace {

struct LuaStateDeleter {
    void operator()(lua_State* L) const { if (L) lua_close(L); }
};
using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleter>;

SchemaType parse_type(const std::string& s) {
    if (s == "string")  return SchemaType::String;
    if (s == "integer") return SchemaType::Integer;
    if (s == "float")   return SchemaType::Float;
    if (s == "boolean") return SchemaType::Boolean;
    if (s == "opaque")  return SchemaType::Opaque;
    return SchemaType::Any;
}

/// Pull a Lua value at stack index `idx` into a Value, picking the
/// narrowest variant alternative that fits. Returns monostate for
/// unsupported types (tables, functions, userdata).
Value lua_value_to_value(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return Value(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                lua_Integer n = lua_tointeger(L, idx);
                if (n >= 0 &&
                    n <= static_cast<lua_Integer>(
                             std::numeric_limits<std::uint32_t>::max())) {
                    return Value(static_cast<std::uint32_t>(n));
                }
                if (n >= std::numeric_limits<std::int32_t>::min() &&
                    n <= std::numeric_limits<std::int32_t>::max()) {
                    return Value(static_cast<std::int32_t>(n));
                }
                return Value(static_cast<double>(n));
            }
            return Value(lua_tonumber(L, idx));
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return Value(std::string(s, len));
        }
        default:
            return Value(std::monostate{});
    }
}

/// Best-effort: stringify a Value for inclusion in a diagnostic
/// message. Booleans → true/false; numbers → decimal; strings bare.
std::string value_diag(const Value& v) {
    return std::visit([](auto&& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "null";
        else if constexpr (std::is_same_v<T, bool>)      return a ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return a;
        else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", a);
            return buf;
        } else {
            return std::to_string(a);
        }
    }, v);
}

/// Pull an integer out of a numeric Value (uint32/int32/double in
/// integer range). Returns nullopt for strings / bools / fractional
/// doubles.
std::optional<long long> value_as_integer(const Value& v) {
    if (std::holds_alternative<std::uint32_t>(v)) {
        return static_cast<long long>(std::get<std::uint32_t>(v));
    }
    if (std::holds_alternative<std::int32_t>(v)) {
        return static_cast<long long>(std::get<std::int32_t>(v));
    }
    if (std::holds_alternative<double>(v)) {
        double d = std::get<double>(v);
        long long n = static_cast<long long>(d);
        if (static_cast<double>(n) == d) return n;
    }
    return std::nullopt;
}

} // namespace

void SchemaRegistry::load_one(const std::string& path) {
    LuaStatePtr L(luaL_newstate());
    if (!L) throw std::runtime_error("luaL_newstate failed");
    luaL_openlibs(L.get());

    if (luaL_dofile(L.get(), path.c_str()) != LUA_OK) {
        std::string err = lua_tostring(L.get(), -1);
        throw std::runtime_error("luaL_dofile: " + err);
    }
    if (!lua_istable(L.get(), -1)) {
        throw std::runtime_error("top-level return is not a table");
    }

    // Capture the `namespace` declaration so validate_set can reject
    // sets on undeclared keys whose namespace IS claimed by some
    // loaded schema (the "services.ds.enable is intentionally absent"
    // pattern from L16/D2). Missing namespace = no claim.
    lua_getfield(L.get(), -1, "namespace");
    if (lua_isstring(L.get(), -1)) {
        std::size_t nlen = 0;
        const char* nc = lua_tolstring(L.get(), -1, &nlen);
        m_namespaces.emplace(nc, nlen);
    }
    lua_pop(L.get(), 1);

    lua_getfield(L.get(), -1, "keys");
    if (!lua_istable(L.get(), -1)) {
        throw std::runtime_error("schema missing `keys` table");
    }

    // Iterate `keys`.
    lua_pushnil(L.get());
    while (lua_next(L.get(), -2) != 0) {
        // key at -2, spec table at -1
        if (lua_type(L.get(), -2) != LUA_TSTRING ||
            !lua_istable(L.get(), -1)) {
            lua_pop(L.get(), 1);
            continue;
        }
        std::size_t klen = 0;
        const char* kc = lua_tolstring(L.get(), -2, &klen);
        std::string key(kc, klen);

        SchemaEntry e;

        lua_getfield(L.get(), -1, "type");
        if (lua_isstring(L.get(), -1)) {
            std::size_t tlen = 0;
            const char* ts = lua_tolstring(L.get(), -1, &tlen);
            e.type = parse_type(std::string(ts, tlen));
        }
        lua_pop(L.get(), 1);

        lua_getfield(L.get(), -1, "default");
        if (!lua_isnil(L.get(), -1)) {
            e.default_value = lua_value_to_value(L.get(), lua_gettop(L.get()));
        }
        lua_pop(L.get(), 1);

        lua_getfield(L.get(), -1, "min");
        if (lua_isnumber(L.get(), -1)) {
            e.min_int = static_cast<long long>(lua_tointeger(L.get(), -1));
        }
        lua_pop(L.get(), 1);

        lua_getfield(L.get(), -1, "max");
        if (lua_isnumber(L.get(), -1)) {
            e.max_int = static_cast<long long>(lua_tointeger(L.get(), -1));
        }
        lua_pop(L.get(), 1);

        auto [it, inserted] = m_entries.emplace(key, e);
        if (!inserted) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [Schema:%t] %M %N:%l duplicate key '%C' "
                                "in %C — overriding previous definition\n"),
                       key.c_str(), path.c_str()));
            it->second = e;
        }
        lua_pop(L.get(), 1);    // spec table
    }
}

std::size_t SchemaRegistry::load_directory(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;       // missing dir is fine

    while (auto* ent = ::readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() < 5) continue;
        if (name.compare(name.size() - 4, 4, ".lua") != 0) continue;
        std::string path = dir;
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path += name;

        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        try {
            load_one(path);
        } catch (const std::exception& e) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [Schema:%t] %M %N:%l skipping %C: %C\n"),
                       path.c_str(), e.what()));
        }
    }
    ::closedir(d);
    return m_entries.size();
}

const SchemaEntry* SchemaRegistry::find(const std::string& key) const {
    auto it = m_entries.find(key);
    return (it == m_entries.end()) ? nullptr : &it->second;
}

std::string SchemaRegistry::first_segment(const std::string& key) {
    auto dot = key.find('.');
    if (dot == std::string::npos) return {};
    return key.substr(0, dot);
}

bool SchemaRegistry::is_namespace_claimed(const std::string& key) const {
    auto ns = first_segment(key);
    if (ns.empty()) return false;
    return m_namespaces.count(ns) > 0;
}

std::optional<std::string> SchemaRegistry::validate_set(
        const std::string& key, const Value& value) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        // Key not declared. If its namespace IS claimed by some
        // loaded schema, reject — every owner is expected to fully
        // enumerate its surface (L16/D2 "intentionally absent
        // services.ds.enable" pattern). Otherwise passthrough
        // (REQ-DS-023 unknown-keys-pass).
        if (is_namespace_claimed(key)) {
            return "schema(" + key + "): namespace '" +
                   first_segment(key) +
                   "' is claimed but this key is not declared";
        }
        return std::nullopt;
    }
    const SchemaEntry& e = it->second;

    // monostate (null) is always accepted — used to clear a key.
    if (std::holds_alternative<std::monostate>(value)) return std::nullopt;

    switch (e.type) {
        case SchemaType::Any:
            return std::nullopt;

        case SchemaType::String:
        case SchemaType::Opaque:
            if (std::holds_alternative<std::string>(value)) return std::nullopt;
            return "schema(" + key + "): expected string, got " +
                   value_diag(value);

        case SchemaType::Boolean:
            if (std::holds_alternative<bool>(value)) return std::nullopt;
            return "schema(" + key + "): expected boolean, got " +
                   value_diag(value);

        case SchemaType::Integer: {
            auto n = value_as_integer(value);
            if (!n) {
                return "schema(" + key + "): expected integer, got " +
                       value_diag(value);
            }
            if (e.min_int && *n < *e.min_int) {
                return "schema(" + key + "): " + std::to_string(*n) +
                       " below min " + std::to_string(*e.min_int);
            }
            if (e.max_int && *n > *e.max_int) {
                return "schema(" + key + "): " + std::to_string(*n) +
                       " above max " + std::to_string(*e.max_int);
            }
            return std::nullopt;
        }

        case SchemaType::Float:
            if (std::holds_alternative<double>(value) ||
                std::holds_alternative<std::uint32_t>(value) ||
                std::holds_alternative<std::int32_t>(value)) {
                return std::nullopt;
            }
            return "schema(" + key + "): expected float, got " +
                   value_diag(value);
    }
    return std::nullopt;
}

std::optional<Value> SchemaRegistry::default_for(
        const std::string& key) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return std::nullopt;
    return it->second.default_value;
}

} // namespace data_store::server
