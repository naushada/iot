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

#include "../src/server/data_store.hpp"
#include "../src/server/lua_persistor.hpp"

namespace ds = data_store::server;

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
    std::unordered_map<std::string, std::string> in {
        {"foo", "bar"},
        {"counter", "42"},
        {"k.with.dots", "v"},
        {"tricky", "has \"quotes\" and\nnewline"},
    };
    p.save(in);

    auto out = p.load();
    EXPECT_EQ(in, out);

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

        s.set("a", "1");
        s.set("b", "2");
        s.remove("a");
        // s.set with unchanged value MUST NOT add a flush — but
        // since the flush is best-effort we just check final state.
        s.set("b", "2");
    }

    {
        ds::LuaPersistor p(path);
        ds::DataStore s;
        s.load_from(p.load());
        EXPECT_EQ(1u, s.size());
        auto b = s.get("b");
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ("2", *b);
        EXPECT_FALSE(s.get("a").has_value());
    }

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}
