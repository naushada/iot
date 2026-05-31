#include "lua_persistor.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <variant>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace data_store::server {

namespace {

constexpr int kSchemaVersion = 2;

struct LuaStateDeleter {
    void operator()(lua_State* L) const { if (L) lua_close(L); }
};
using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleter>;

/// Lua-quote a string: wrap in `"..."`, escape `\`, `"`, and any
/// control char as `\xNN`. Keys are quoted with `[" ... "]` syntax;
/// values bare-quoted with `"..."`.
std::string lua_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%d", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

/// Render a Value as its Lua literal form.
std::string serialise_value(const Value& v) {
    return std::visit([](auto&& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "nil";
        } else if constexpr (std::is_same_v<T, bool>) {
            return a ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return lua_quote(a);
        } else if constexpr (std::is_same_v<T, std::uint32_t> ||
                             std::is_same_v<T, std::int32_t>) {
            return std::to_string(a);
        } else if constexpr (std::is_same_v<T, double>) {
            // Lua 5.3+ accepts e-notation; round-trip with %.17g.
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", a);
            return std::string(buf);
        } else {
            return "nil";
        }
    }, v);
}

/// Pull a single Lua-stack value (idx is absolute) into a Value.
/// Returns std::nullopt when the type isn't representable in Value
/// (tables, functions, userdata) — caller skips those entries.
std::optional<Value> read_value(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            return Value(std::monostate{});
        case LUA_TBOOLEAN:
            return Value(lua_toboolean(L, idx) != 0);
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return Value(std::string(s, len));
        }
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
        default:
            return std::nullopt;
    }
}

} // namespace

LuaPersistor::LuaPersistor(std::string path) : m_path(std::move(path)) {}

std::unordered_map<std::string, Value> LuaPersistor::load() {
    std::unordered_map<std::string, Value> out;

    // Missing file is fine — start with an empty map.
    if (::access(m_path.c_str(), R_OK) != 0) return out;

    LuaStatePtr L(luaL_newstate());
    if (!L) throw CorruptStoreError("luaL_newstate failed");
    luaL_openlibs(L.get());

    if (luaL_dofile(L.get(), m_path.c_str()) != LUA_OK) {
        const char* err = lua_tostring(L.get(), -1);
        throw CorruptStoreError(std::string("luaL_dofile: ") +
                                (err ? err : "(unknown)"));
    }
    if (!lua_istable(L.get(), -1)) {
        throw CorruptStoreError("top-level return is not a table");
    }

    // schema_version is informational — both v1 (string-only) and v2
    // (typed) parse correctly through the value-type switch below.
    lua_getfield(L.get(), -1, "schema_version");
    lua_pop(L.get(), 1);

    lua_getfield(L.get(), -1, "data");
    if (!lua_istable(L.get(), -1)) {
        throw CorruptStoreError("missing or non-table `data` field");
    }

    lua_pushnil(L.get());
    while (lua_next(L.get(), -2) != 0) {
        if (lua_type(L.get(), -2) == LUA_TSTRING) {
            std::size_t klen = 0;
            const char* k = lua_tolstring(L.get(), -2, &klen);
            auto val = read_value(L.get(), lua_gettop(L.get()));
            if (val) out.emplace(std::string(k, klen), std::move(*val));
        }
        lua_pop(L.get(), 1);
    }
    return out;
}

void LuaPersistor::save(
        const std::unordered_map<std::string, Value>& data) {
    // Build the serialised text first so a serialise error doesn't
    // leave a half-written tmp file behind.
    std::ostringstream ss;
    ss << "return {\n";
    ss << "  schema_version = " << kSchemaVersion << ",\n";
    ss << "  data = {\n";
    for (const auto& [k, v] : data) {
        // monostate keys serialise to `nil` — Lua would drop them on
        // load, so we skip them here for a smaller, cleaner file.
        if (std::holds_alternative<std::monostate>(v)) continue;
        ss << "    [" << lua_quote(k) << "] = "
           << serialise_value(v) << ",\n";
    }
    ss << "  },\n";
    ss << "}\n";
    const std::string body = ss.str();

    const std::string tmp = m_path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        throw std::runtime_error(std::string("open(") + tmp + "): " +
                                 std::strerror(errno));
    }
    ssize_t wrote = 0;
    while (wrote < static_cast<ssize_t>(body.size())) {
        ssize_t n = ::write(fd, body.data() + wrote,
                            body.size() - static_cast<std::size_t>(wrote));
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno; ::close(fd); ::unlink(tmp.c_str());
            throw std::runtime_error(std::string("write: ") +
                                     std::strerror(e));
        }
        wrote += n;
    }
    if (::fsync(fd) < 0) {
        int e = errno; ::close(fd); ::unlink(tmp.c_str());
        throw std::runtime_error(std::string("fsync: ") + std::strerror(e));
    }
    ::close(fd);
    if (::rename(tmp.c_str(), m_path.c_str()) < 0) {
        int e = errno; ::unlink(tmp.c_str());
        throw std::runtime_error(std::string("rename(") + tmp + " → " +
                                 m_path + "): " + std::strerror(e));
    }
}

} // namespace data_store::server
