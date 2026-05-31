#include "schema.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>

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

/// Stringify a Lua value at stack index `idx` to the wire form
/// (everything in the store is a std::string).
std::string lua_value_to_string(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "1" : "0";
        case LUA_TNUMBER: {
            double n = lua_tonumber(L, idx);
            // Integer-friendly print when the value is a whole number.
            if (n == static_cast<double>(static_cast<long long>(n))) {
                return std::to_string(static_cast<long long>(n));
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", n);
            return buf;
        }
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return std::string(s, len);
        }
        default: return {};
    }
}

bool parse_long(const std::string& s, long long& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        out = std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
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
            e.default_value = lua_value_to_string(L.get(), -1);
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

std::optional<std::string> SchemaRegistry::validate_set(
        const std::string& key, const std::string& value) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return std::nullopt;  // passthrough
    const SchemaEntry& e = it->second;
    switch (e.type) {
        case SchemaType::Any:
        case SchemaType::String:
        case SchemaType::Opaque:
            return std::nullopt;
        case SchemaType::Boolean: {
            if (value == "0" || value == "1" ||
                value == "true" || value == "false") return std::nullopt;
            return "schema(" + key + "): expected boolean, got '" + value + "'";
        }
        case SchemaType::Integer: {
            long long n;
            if (!parse_long(value, n)) {
                return "schema(" + key + "): expected integer, got '" + value + "'";
            }
            if (e.min_int && n < *e.min_int) {
                return "schema(" + key + "): " + std::to_string(n) +
                       " below min " + std::to_string(*e.min_int);
            }
            if (e.max_int && n > *e.max_int) {
                return "schema(" + key + "): " + std::to_string(n) +
                       " above max " + std::to_string(*e.max_int);
            }
            return std::nullopt;
        }
        case SchemaType::Float: {
            double f;
            if (!parse_double(value, f)) {
                return "schema(" + key + "): expected float, got '" + value + "'";
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> SchemaRegistry::default_for(
        const std::string& key) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return std::nullopt;
    return it->second.default_value;
}

} // namespace data_store::server
