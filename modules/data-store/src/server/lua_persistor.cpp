#include "lua_persistor.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

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

} // namespace

LuaPersistor::LuaPersistor(std::string path) : m_path(std::move(path)) {}

std::unordered_map<std::string, std::string> LuaPersistor::load() {
    std::unordered_map<std::string, std::string> out;

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

    // Pull the schema_version (currently informational; we accept
    // any version since v1 is forward-compatible with v0).
    lua_getfield(L.get(), -1, "schema_version");
    lua_pop(L.get(), 1);

    lua_getfield(L.get(), -1, "data");
    if (!lua_istable(L.get(), -1)) {
        throw CorruptStoreError("missing or non-table `data` field");
    }

    // Walk `data` — keys + values both strings.
    lua_pushnil(L.get());
    while (lua_next(L.get(), -2) != 0) {
        if (lua_type(L.get(), -2) == LUA_TSTRING &&
            lua_type(L.get(), -1) == LUA_TSTRING) {
            std::size_t klen = 0, vlen = 0;
            const char* k = lua_tolstring(L.get(), -2, &klen);
            const char* v = lua_tolstring(L.get(), -1, &vlen);
            out.emplace(std::string(k, klen), std::string(v, vlen));
        }
        lua_pop(L.get(), 1);
    }
    return out;
}

void LuaPersistor::save(
        const std::unordered_map<std::string, std::string>& data) {
    // Build the serialised text first so a serialise error doesn't
    // leave a half-written tmp file behind.
    std::ostringstream ss;
    ss << "return {\n";
    ss << "  schema_version = 1,\n";
    ss << "  data = {\n";
    for (const auto& [k, v] : data) {
        ss << "    [" << lua_quote(k) << "] = " << lua_quote(v) << ",\n";
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
