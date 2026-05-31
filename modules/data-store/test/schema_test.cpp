/// SchemaRegistry + Worker integration tests.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "data_store/value.hpp"
#include "../src/server/schema.hpp"

namespace ds = data_store::server;
using data_store::Value;

namespace {

std::string make_tmpdir() {
    char tmpl[64] = "/tmp/ds-schema-XXXXXX";
    if (char* d = mkdtemp(tmpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tmpl, "/tmp/ds-schema-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    std::strcpy(tmpl, "./ds-schema-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    return {};
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream(path) << body;
}

} // namespace

TEST(Schema, missing_dir_yields_empty_registry) {
    ds::SchemaRegistry r;
    EXPECT_EQ(0u, r.load_directory("/no/such/dir"));
    EXPECT_EQ(0u, r.size());
}

TEST(Schema, loads_single_file_and_indexes_keys) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    write_file(dir + "/iot.lua", R"(
return {
  namespace = "iot",
  keys = {
    ["iot.lifetime"]  = { type="integer", default=86400, min=0 },
    ["iot.endpoint"]  = { type="string",  default="urn:dev:client-1" },
  },
})");

    ds::SchemaRegistry r;
    EXPECT_EQ(2u, r.load_directory(dir));
    auto* e = r.find("iot.lifetime");
    ASSERT_NE(nullptr, e);
    EXPECT_EQ(ds::SchemaType::Integer, e->type);
    ASSERT_TRUE(e->default_value.has_value());
    EXPECT_EQ(86400u, std::get<std::uint32_t>(*e->default_value));
    EXPECT_EQ(0LL, e->min_int.value_or(-1));

    EXPECT_EQ(nullptr, r.find("not.in.schema"));
    ::unlink((dir + "/iot.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, validate_set_passes_unknown_keys_through) {
    ds::SchemaRegistry r;
    // No schema loaded → every set passes.
    EXPECT_FALSE(r.validate_set("anything",
        Value{std::string("v")}).has_value());
}

TEST(Schema, validate_set_rejects_type_mismatch) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    write_file(dir + "/x.lua", R"(
return { keys = { ["n"] = { type="integer", min=0, max=10 } } })");

    ds::SchemaRegistry r;
    r.load_directory(dir);
    EXPECT_FALSE(r.validate_set("n",
        Value{static_cast<std::uint32_t>(5)}).has_value());     // ok
    EXPECT_TRUE (r.validate_set("n",
        Value{std::string("foo")}).has_value());                 // not integer
    EXPECT_TRUE (r.validate_set("n",
        Value{static_cast<std::int32_t>(-1)}).has_value());      // below min
    EXPECT_TRUE (r.validate_set("n",
        Value{static_cast<std::uint32_t>(99)}).has_value());     // above max
    ::unlink((dir + "/x.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, validate_set_typed_string_and_boolean) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    write_file(dir + "/t.lua", R"(
return { keys = {
    ["name"]    = { type="string"  },
    ["enabled"] = { type="boolean" },
} })");
    ds::SchemaRegistry r;
    r.load_directory(dir);
    EXPECT_FALSE(r.validate_set("name",
        Value{std::string("ok")}).has_value());
    EXPECT_TRUE (r.validate_set("name",
        Value{static_cast<std::uint32_t>(1)}).has_value());
    EXPECT_FALSE(r.validate_set("enabled", Value{true}).has_value());
    EXPECT_TRUE (r.validate_set("enabled",
        Value{std::string("true")}).has_value());
    ::unlink((dir + "/t.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, default_for_returns_typed_default) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    write_file(dir + "/d.lua", R"(
return {
  keys = {
    ["a"] = { type="integer", default=42 },
    ["b"] = { type="boolean", default=true },
    ["c"] = { type="string",  default="hello" },
    ["d"] = { type="integer" },     -- no default
  },
})");

    ds::SchemaRegistry r;
    r.load_directory(dir);
    auto a = r.default_for("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(42u, std::get<std::uint32_t>(*a));
    auto b = r.default_for("b");
    ASSERT_TRUE(b.has_value());
    EXPECT_TRUE(std::get<bool>(*b));
    auto c = r.default_for("c");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(std::string("hello"), std::get<std::string>(*c));
    EXPECT_FALSE(r.default_for("d").has_value());
    EXPECT_FALSE(r.default_for("nope").has_value());
    ::unlink((dir + "/d.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, duplicate_key_across_files_last_wins) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    write_file(dir + "/a.lua", R"(
return { keys = { ["dup"] = { type="integer", default=1 } } })");
    write_file(dir + "/b.lua", R"(
return { keys = { ["dup"] = { type="integer", default=2 } } })");

    ds::SchemaRegistry r;
    EXPECT_EQ(1u, r.load_directory(dir));      // one unique key
    auto* e = r.find("dup");
    ASSERT_NE(nullptr, e);
    ASSERT_TRUE(e->default_value.has_value());
    // Last-loaded wins; filesystem readdir order isn't guaranteed,
    // so just assert it's one of the two.
    auto n = std::get<std::uint32_t>(*e->default_value);
    EXPECT_TRUE(n == 1u || n == 2u);

    ::unlink((dir + "/a.lua").c_str());
    ::unlink((dir + "/b.lua").c_str());
    ::rmdir(dir.c_str());
}
