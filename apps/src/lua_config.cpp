#include "lua_config.hpp"

#include <cmath>
#include <memory>

#include <ace/Log_Msg.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace iot::lua_config {

namespace {

struct LuaStateDeleter {
    void operator()(lua_State* L) const { if (L) lua_close(L); }
};
using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleter>;

/// Extract a primitive value at the given absolute stack index. Sub-tables
/// of the shape `{ bytes = {...}, subtype = N }` collapse to OpaqueBytes;
/// other table shapes return monostate (unsupported).
ResourceValue extract_value(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return static_cast<bool>(lua_toboolean(L, idx));
        case LUA_TNUMBER: {
            // Distinguish int vs float by the fractional part. Lua 5.3+
            // would let us use lua_isinteger; we stick to a portable check
            // so the loader works against 5.1/5.2 too.
            double n = lua_tonumber(L, idx);
            double i;
            if (std::modf(n, &i) == 0.0) {
                return static_cast<long long>(n);
            }
            return n;
        }
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return std::string(s, len);
        }
        case LUA_TTABLE: {
            // Recognise the opaque shape { bytes = {...}, subtype = N }.
            lua_getfield(L, idx, "bytes");
            if (lua_istable(L, -1)) {
                OpaqueBytes bytes;
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_isnumber(L, -1)) {
                        bytes.push_back(
                            static_cast<std::uint8_t>(lua_tointeger(L, -1) & 0xFF));
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // bytes table
                return bytes;
            }
            lua_pop(L, 1);
            return std::monostate{};
        }
        default:
            return std::monostate{};
    }
}

ResourceRecord extract_record(lua_State* L, int idx) {
    ResourceRecord r;

    lua_getfield(L, idx, "description");
    if (lua_isstring(L, -1)) {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        r.description.assign(s, len);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "include");
    r.include = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : true;
    lua_pop(L, 1);

    lua_getfield(L, idx, "value");
    r.value = extract_value(L, lua_gettop(L));
    lua_pop(L, 1);

    return r;
}

} // namespace

ResourceMap load_object_resources(const std::string& path) {
    ResourceMap out;
    LuaStatePtr L(luaL_newstate());
    if (!L) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [iot:%t] %M %N:%l lua_config %C: luaL_newstate failed\n"),
                   path.c_str()));
        return out;
    }
    luaL_openlibs(L.get());

    if (luaL_dofile(L.get(), path.c_str()) != LUA_OK) {
        // Missing file is silent; other errors surface so a malformed
        // config doesn't hide behind the empty-map fallback.
        const char* err = lua_tostring(L.get(), -1);
        std::string msg = err ? err : "(no error message)";
        if (msg.find("cannot open") == std::string::npos) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [iot:%t] %M %N:%l lua_config %C: %C\n"),
                       path.c_str(), msg.c_str()));
        }
        return out;
    }

    if (!lua_istable(L.get(), -1)) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [iot:%t] %M %N:%l lua_config %C: top-level return is not a table\n"),
                   path.c_str()));
        return out;
    }

    // Walk the single top-level entry — the file may use any name
    // (deviceObject / serverObject / securityObject); we don't enforce.
    lua_pushnil(L.get());
    if (lua_next(L.get(), -2) == 0) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [iot:%t] %M %N:%l lua_config %C: top-level table is empty\n"),
                   path.c_str()));
        return out;
    }
    // Stack: top-level table (-3), key (-2), object-table (-1)
    if (!lua_istable(L.get(), -1)) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [iot:%t] %M %N:%l lua_config %C: object value is not a table\n"),
                   path.c_str()));
        lua_pop(L.get(), 2);
        return out;
    }

    lua_getfield(L.get(), -1, "resources");
    if (!lua_istable(L.get(), -1)) {
        // No resources sub-table: treat as empty rather than error.
        lua_pop(L.get(), 3);
        return out;
    }

    // Iterate resources: keys are integer RIDs, values are records.
    lua_pushnil(L.get());
    while (lua_next(L.get(), -2) != 0) {
        // Stack: ..., resources (-3), rid-key (-2), record-table (-1)
        if (lua_isnumber(L.get(), -2) && lua_istable(L.get(), -1)) {
            std::uint32_t rid =
                static_cast<std::uint32_t>(lua_tointeger(L.get(), -2));
            out.emplace(rid, extract_record(L.get(), lua_gettop(L.get())));
        }
        lua_pop(L.get(), 1); // record-table; rid-key stays for next iteration
    }

    // Cleanup: resources(-1), object-table(-2 was at -1 before), key, top
    // — easier just to clear to the bottom.
    lua_settop(L.get(), 0);
    return out;
}

// ----- typed accessors -----

std::string string_or(const ResourceMap& m, std::uint32_t rid,
                      const std::string& def) {
    auto it = m.find(rid);
    if (it == m.end() || !it->second.include) return def;
    if (auto* s = std::get_if<std::string>(&it->second.value)) return *s;
    return def;
}

std::uint32_t uint_or(const ResourceMap& m, std::uint32_t rid,
                      std::uint32_t def) {
    auto it = m.find(rid);
    if (it == m.end() || !it->second.include) return def;
    if (auto* n = std::get_if<long long>(&it->second.value)) {
        if (*n < 0) return def;
        return static_cast<std::uint32_t>(*n);
    }
    return def;
}

bool bool_or(const ResourceMap& m, std::uint32_t rid, bool def) {
    auto it = m.find(rid);
    if (it == m.end() || !it->second.include) return def;
    if (auto* b = std::get_if<bool>(&it->second.value)) return *b;
    return def;
}

} // namespace iot::lua_config
