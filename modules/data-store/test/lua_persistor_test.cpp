/// LuaPersistor unit tests + end-to-end persistence round-trip via
/// DataStore::set_persistor.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "data_store/value.hpp"
#include "../src/server/data_store.hpp"
#include "../src/server/lua_persistor.hpp"

namespace ds = data_store::server;
using data_store::Value;

namespace {

std::string make_tmpdir() {
    char tmpl[64] = "/tmp/ds-persist-XXXXXX";
    if (char* d = mkdtemp(tmpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tmpl, "/tmp/ds-persist-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    std::strcpy(tmpl, "./ds-persist-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    return {};
}

} // namespace

TEST(LuaPersistor, load_returns_empty_when_file_missing) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    ds::LuaPersistor p(dir + "/missing.lua");
    EXPECT_TRUE(p.load().empty());
    ::rmdir(dir.c_str());
}

TEST(LuaPersistor, save_then_load_round_trips) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string path = dir + "/store.lua";

    ds::LuaPersistor p(path);
    std::unordered_map<std::string, Value> in {
        {"foo",          Value{std::string("bar")}},
        {"counter",      Value{static_cast<std::uint32_t>(42)}},
        {"signed",       Value{static_cast<std::int32_t>(-7)}},
        {"ratio",        Value{1.5}},
        {"enabled",      Value{true}},
        {"k.with.dots",  Value{std::string("v")}},
        {"tricky",       Value{std::string("has \"quotes\" and\nnewline")}},
    };
    p.save(in);

    auto out = p.load();
    EXPECT_EQ(in, out);

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

TEST(LuaPersistor, v1_string_only_file_loads_as_strings) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string path = dir + "/v1.lua";

    // Hand-crafted v1 file — every value is a Lua string literal,
    // schema_version=1. Must load cleanly under the new typed code.
    std::ofstream(path) <<
        "return {\n"
        "  schema_version = 1,\n"
        "  data = {\n"
        "    [\"foo\"] = \"bar\",\n"
        "    [\"baz\"] = \"qux\",\n"
        "  },\n"
        "}\n";

    ds::LuaPersistor p(path);
    auto out = p.load();
    ASSERT_EQ(2u, out.size());
    EXPECT_EQ(std::string("bar"), std::get<std::string>(out.at("foo")));
    EXPECT_EQ(std::string("qux"), std::get<std::string>(out.at("baz")));

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

TEST(LuaPersistor, corrupted_file_throws_CorruptStoreError) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string path = dir + "/bad.lua";
    std::ofstream(path) << "this is not lua";

    ds::LuaPersistor p(path);
    EXPECT_THROW(p.load(), ds::CorruptStoreError);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

TEST(LuaPersistor, missing_data_table_throws) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string path = dir + "/no-data.lua";
    std::ofstream(path) << "return { schema_version = 1 }\n";

    ds::LuaPersistor p(path);
    EXPECT_THROW(p.load(), ds::CorruptStoreError);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

/* ─────────────────────── DataStore + persistor ────────────────── */

TEST(DataStorePersist, set_flushes_to_disk_and_load_restores) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string path = dir + "/store.lua";

    {
        ds::LuaPersistor p(path);
        ds::DataStore s;
        s.set_persistor(&p);

        s.set("a", Value{std::string("1")});
        s.set("b", Value{static_cast<std::uint32_t>(2)});
        s.remove("a");
        // s.set with unchanged value MUST NOT add a flush — but
        // since the flush is best-effort we just check final state.
        s.set("b", Value{static_cast<std::uint32_t>(2)});
    }

    {
        ds::LuaPersistor p(path);
        ds::DataStore s;
        s.load_from(p.load());
        EXPECT_EQ(1u, s.size());
        auto b = s.get("b");
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(2u, std::get<std::uint32_t>(*b));
        EXPECT_FALSE(s.get("a").has_value());
    }

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}
