#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "oci_bundle.hpp"

using namespace containers;
using json = nlohmann::json;

// ── image config parse ─────────────────────────────────────────────────────
TEST(ImageConfig, ParsesNestedConfig) {
    const char* body = R"({
      "architecture":"arm64","os":"linux",
      "config":{
        "Env":["PATH=/usr/bin","FOO=bar"],
        "Entrypoint":["/entry.sh"],
        "Cmd":["-g","daemon off;"],
        "WorkingDir":"/app",
        "User":"1000:2000"
      }
    })";
    auto c = parse_image_config(body);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.env.size(), 2u);
    EXPECT_EQ(c.entrypoint, (std::vector<std::string>{"/entry.sh"}));
    EXPECT_EQ(c.cmd, (std::vector<std::string>{"-g", "daemon off;"}));
    EXPECT_EQ(c.working_dir, "/app");
    EXPECT_EQ(c.user, "1000:2000");
}

TEST(ImageConfig, GarbageNotOk) {
    EXPECT_FALSE(parse_image_config("nope").ok);
}

// ── args field parse ───────────────────────────────────────────────────────
TEST(ArgsField, JsonArray) {
    EXPECT_EQ(parse_args_field(R"(["nginx","-g","daemon off;"])"),
              (std::vector<std::string>{"nginx", "-g", "daemon off;"}));
}
TEST(ArgsField, WhitespaceSplit) {
    EXPECT_EQ(parse_args_field("echo hello world"),
              (std::vector<std::string>{"echo", "hello", "world"}));
}
TEST(ArgsField, EmptyIsEmpty) {
    EXPECT_TRUE(parse_args_field("   ").empty());
}

// ── arg resolution ─────────────────────────────────────────────────────────
TEST(ResolveArgs, ImageDefaults) {
    auto a = resolve_process_args({"/entry"}, {"--default"}, {}, false, {}, false);
    EXPECT_EQ(a, (std::vector<std::string>{"/entry", "--default"}));
}
TEST(ResolveArgs, CmdOverrideOnly) {
    // `docker run img CMD...` — image entrypoint + override cmd.
    auto a = resolve_process_args({"/entry"}, {"--default"}, {}, false, {"run", "now"}, true);
    EXPECT_EQ(a, (std::vector<std::string>{"/entry", "run", "now"}));
}
TEST(ResolveArgs, EntrypointOverride) {
    auto a = resolve_process_args({"/entry"}, {"--default"}, {"/sh"}, true, {}, false);
    EXPECT_EQ(a, (std::vector<std::string>{"/sh", "--default"}));
}

// ── limits ─────────────────────────────────────────────────────────────────
TEST(Limits, Memory) {
    EXPECT_EQ(parse_mem_limit("256M"), 256LL * 1024 * 1024);
    EXPECT_EQ(parse_mem_limit("1g"), 1024LL * 1024 * 1024);
    EXPECT_EQ(parse_mem_limit("536870912"), 536870912LL);
    EXPECT_EQ(parse_mem_limit(""), 0);
    EXPECT_EQ(parse_mem_limit("lots"), 0);
}
TEST(Limits, CpuQuota) {
    EXPECT_EQ(parse_cpu_quota("0.5", 100000), 50000);
    EXPECT_EQ(parse_cpu_quota("2", 100000), 200000);
    EXPECT_EQ(parse_cpu_quota("", 100000), 0);
    EXPECT_EQ(parse_cpu_quota("0", 100000), 0);
}
TEST(Limits, User) {
    unsigned uid = 9, gid = 9;
    resolve_user("1000:2000", uid, gid);
    EXPECT_EQ(uid, 1000u);
    EXPECT_EQ(gid, 2000u);
    resolve_user("1000", uid, gid);
    EXPECT_EQ(uid, 1000u);
    EXPECT_EQ(gid, 0u);
    resolve_user("root", uid, gid);   // named → 0:0 (v1)
    EXPECT_EQ(uid, 0u);
    EXPECT_EQ(gid, 0u);
}

// ── config.json generation ─────────────────────────────────────────────────
TEST(OciConfig, HostNetAndArgsAndLimits) {
    OciSpec s;
    s.args = {"/bin/sh", "-c", "echo hi"};
    s.env = {"FOO=bar"};
    s.cwd = "/app";
    s.uid = 1000;
    s.gid = 1000;
    s.mem_limit_bytes = 256LL * 1024 * 1024;
    s.cpu_quota = 50000;
    auto out = generate_oci_config(s);
    auto j = json::parse(out);

    EXPECT_EQ(j["process"]["args"], json::array({"/bin/sh", "-c", "echo hi"}));
    EXPECT_EQ(j["process"]["cwd"], "/app");
    EXPECT_EQ(j["process"]["user"]["uid"], 1000);
    EXPECT_EQ(j["root"]["path"], "rootfs");
    EXPECT_EQ(j["process"]["noNewPrivileges"], true);

    // host networking: there must be NO network namespace.
    bool has_net = false;
    for (const auto& n : j["linux"]["namespaces"])
        if (n["type"] == "network") has_net = true;
    EXPECT_FALSE(has_net);

    EXPECT_EQ(j["linux"]["resources"]["memory"]["limit"], 256LL * 1024 * 1024);
    EXPECT_EQ(j["linux"]["resources"]["cpu"]["quota"], 50000);
    EXPECT_EQ(j["linux"]["resources"]["cpu"]["period"], 100000);
}

TEST(OciConfig, BridgeModeAddsNetworkNamespace) {
    OciSpec s;
    s.args = {"/bin/true"};
    s.host_network = false;             // bridge mode → own netns
    auto j = json::parse(generate_oci_config(s));
    bool has_net = false;
    for (const auto& n : j["linux"]["namespaces"])
        if (n["type"] == "network") has_net = true;
    EXPECT_TRUE(has_net);
}

TEST(OciConfig, NoLimitsOmitsResources) {
    OciSpec s;
    s.args = {"/bin/true"};
    auto j = json::parse(generate_oci_config(s));
    // resources object absent (or without memory/cpu) when unbounded.
    if (j["linux"].contains("resources")) {
        EXPECT_FALSE(j["linux"]["resources"].contains("memory"));
        EXPECT_FALSE(j["linux"]["resources"].contains("cpu"));
    }
    // empty env → a default PATH is injected.
    bool has_path = false;
    for (const auto& e : j["process"]["env"])
        if (e.get<std::string>().rfind("PATH=", 0) == 0) has_path = true;
    EXPECT_TRUE(has_path);
}
