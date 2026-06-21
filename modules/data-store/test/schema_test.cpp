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

// ─────────────────── L16/D2 namespace-claimed rejection ─────────────

TEST(Schema, SVC_REQ_SVC_008_undeclared_key_in_claimed_namespace_rejected) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    // services namespace owned; only services.ds.state declared.
    // services.ds.enable is intentionally absent — set must reject.
    write_file(dir + "/services.lua", R"(
return {
  namespace = "services",
  keys = {
    ["services.ds.state"] = { type = "string", default = "running" },
  },
})");

    ds::SchemaRegistry r;
    ASSERT_EQ(1u, r.load_directory(dir));
    EXPECT_EQ(1u, r.namespace_count());
    EXPECT_TRUE(r.is_namespace_claimed("services.ds.enable"));
    EXPECT_TRUE(r.is_namespace_claimed("services.foo.bar"));
    EXPECT_FALSE(r.is_namespace_claimed("other.key"));

    // A bool set against an undeclared key in a claimed namespace
    // MUST be rejected.
    auto err = r.validate_set("services.ds.enable", Value{true});
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(std::string::npos, err->find("services.ds.enable"));
    EXPECT_NE(std::string::npos, err->find("not declared"));

    // Unrelated namespace still passes through.
    EXPECT_FALSE(r.validate_set("other.foo", Value{std::string("v")})
                     .has_value());

    ::unlink((dir + "/services.lua").c_str());
    ::rmdir(dir.c_str());
}

TEST(Schema, SVC_REQ_SVC_008_declared_key_in_claimed_namespace_accepted) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    write_file(dir + "/services.lua", R"(
return {
  namespace = "services",
  keys = {
    ["services.net.router.enable"] = { type = "boolean", default = true },
  },
})");
    ds::SchemaRegistry r;
    ASSERT_EQ(1u, r.load_directory(dir));
    // Declared key with matching type → accepted.
    EXPECT_FALSE(r.validate_set("services.net.router.enable",
                                Value{false}).has_value());
    ::unlink((dir + "/services.lua").c_str());
    ::rmdir(dir.c_str());
}

// ── PSK provisioning: read_acl enforcement (task C) ────────────────
//
// Mirrors check_write_acl. The write-only PSK keys carry
// read_acl={"gid:engineer"}; only the engineer client (and dev-mode,
// handled in the worker) may read them. ds-cli (root) is denied.

namespace {
// A schema with a read-protected key, mirroring the iot.bs.psk.key
// shape. We use uid:1234 so the test is deterministic without needing
// a real group on the build host.
ds::SchemaRegistry load_psk_schema(const std::string& dir) {
    write_file(dir + "/psk.lua", R"(
return {
  namespace = "iot",
  keys = {
    ["iot.bs.psk.key"] = { type="opaque",
                           write_acl={"uid:1234"},
                           read_acl={"uid:1234"} },
    ["iot.endpoint"]   = { type="string", default="urn:dev:client-1" },
  },
})");
    ds::SchemaRegistry r;
    r.load_directory(dir);
    return r;
}
} // namespace

TEST(Schema, check_read_acl_denies_unlisted_uid) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    auto r = load_psk_schema(dir);
    // root (uid 0) is NOT in read_acl → denied.
    auto err = r.check_read_acl("iot.bs.psk.key", /*uid*/0, /*gid*/0);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(std::string::npos, err->find("read_acl"));
    EXPECT_NE(std::string::npos, err->find("iot.bs.psk.key"));
    ::unlink((dir + "/psk.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, check_read_acl_allows_listed_uid) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    auto r = load_psk_schema(dir);
    EXPECT_FALSE(r.check_read_acl("iot.bs.psk.key", 1234, 0).has_value());
    ::unlink((dir + "/psk.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, check_read_acl_allows_key_without_acl) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    auto r = load_psk_schema(dir);
    // iot.endpoint has no read_acl → unrestricted.
    EXPECT_FALSE(r.check_read_acl("iot.endpoint", 0, 0).has_value());
    ::unlink((dir + "/psk.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, check_read_acl_allows_unknown_key) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    auto r = load_psk_schema(dir);
    // No schema entry at all → passthrough (allowed).
    EXPECT_FALSE(r.check_read_acl("not.in.schema", 0, 0).has_value());
    ::unlink((dir + "/psk.lua").c_str()); ::rmdir(dir.c_str());
}

TEST(Schema, real_iot_lua_declares_psk_keys) {
    // Load the shipped schema and assert the PSK provisioning keys exist
    // with the expected types + ACLs. Try a few candidate paths so the
    // test works from the in-tree build dir or the container /src layout.
    ds::SchemaRegistry r;
    const char* candidates[] = {
        "../schemas",
        "/src/modules/data-store/schemas",
        "schemas",
        "../../schemas",
    };
    bool loaded = false;
    for (const char* p : candidates) {
        if (r.load_directory(p) > 0) { loaded = true; break; }
    }
    if (!loaded) GTEST_SKIP() << "shipped schemas dir not found from cwd";

    struct Expect { const char* key; ds::SchemaType type; bool read_locked; };
    const Expect want[] = {
        {"iot.serial",          ds::SchemaType::String,  false},
        {"iot.dev.mode",        ds::SchemaType::Boolean, false},
        {"iot.bs.psk.identity", ds::SchemaType::String,  true },
        {"iot.bs.psk.key",      ds::SchemaType::Opaque,  true },
        {"iot.bs.psk.override", ds::SchemaType::Boolean, false},
        {"iot.dm.psk.identity", ds::SchemaType::String,  true },
        {"iot.dm.psk.key",      ds::SchemaType::Opaque,  true },
    };
    for (const auto& w : want) {
        auto* e = r.find(w.key);
        ASSERT_NE(nullptr, e) << "missing key " << w.key;
        EXPECT_EQ(w.type, e->type) << w.key;
        EXPECT_FALSE(e->write_acl.empty()) << w.key << " should be write-gated";
        EXPECT_EQ("gid:engineer", e->write_acl[0]) << w.key;
        if (w.read_locked) {
            ASSERT_FALSE(e->read_acl.empty()) << w.key << " should be read-gated";
            EXPECT_EQ("gid:engineer", e->read_acl[0]) << w.key;
        } else {
            EXPECT_TRUE(e->read_acl.empty()) << w.key << " should stay readable";
        }
    }
}

TEST(Schema, parses_both_write_and_read_acl) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    auto r = load_psk_schema(dir);
    auto* e = r.find("iot.bs.psk.key");
    ASSERT_NE(nullptr, e);
    EXPECT_EQ(ds::SchemaType::Opaque, e->type);
    ASSERT_EQ(1u, e->write_acl.size());
    ASSERT_EQ(1u, e->read_acl.size());
    EXPECT_EQ("uid:1234", e->write_acl[0]);
    EXPECT_EQ("uid:1234", e->read_acl[0]);
    ::unlink((dir + "/psk.lua").c_str()); ::rmdir(dir.c_str());
}
