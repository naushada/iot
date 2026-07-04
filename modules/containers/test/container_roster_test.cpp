#include <gtest/gtest.h>

#include "container_roster.hpp"

using namespace containers;

// A representative container.instances document as the daemon publishes it
// (post-fix: carries the config fields subnet/entrypoint/cmd needed to fully
// reconstruct a container on rehydrate).
static const char* kRoster =
    "[{\"name\":\"web\",\"image\":\"docker.io/library/nginx:latest\","
    "\"imageId\":\"sha256:abc\",\"size\":\"1234\",\"state\":\"running\","
    "\"ip\":\"10.88.0.2\",\"gateway\":\"10.88.0.1\",\"net\":\"bridge\","
    "\"subnet\":\"10.88.0.0/24\",\"mem\":\"256M\",\"cpus\":\"0.5\","
    "\"entrypoint\":\"\",\"cmd\":\"[\\\"-g\\\",\\\"daemon off;\\\"]\","
    "\"memUsage\":\"12.3M\",\"cpuPct\":\"3.2%\",\"pid\":1234,"
    "\"exitCode\":null,\"started\":100,\"error\":\"\"},"
    "{\"name\":\"db\",\"image\":\"redis:7\",\"imageId\":\"sha256:def\","
    "\"state\":\"stopped\",\"net\":\"host\",\"mem\":\"\",\"cpus\":\"\"}]";

TEST(Roster, ParsesAllFields) {
    std::vector<RosterEntry> v;
    ASSERT_TRUE(parse_roster(kRoster, v));
    ASSERT_EQ(v.size(), 2u);

    EXPECT_EQ(v[0].name, "web");
    EXPECT_EQ(v[0].image, "docker.io/library/nginx:latest");
    EXPECT_EQ(v[0].image_id, "sha256:abc");
    EXPECT_EQ(v[0].size, "1234");
    EXPECT_EQ(v[0].state, "running");
    EXPECT_EQ(v[0].ip, "10.88.0.2");
    EXPECT_EQ(v[0].gateway, "10.88.0.1");
    EXPECT_EQ(v[0].net, "bridge");
    EXPECT_EQ(v[0].subnet, "10.88.0.0/24");
    EXPECT_EQ(v[0].mem, "256M");
    EXPECT_EQ(v[0].cpus, "0.5");
    EXPECT_EQ(v[0].entrypoint, "");
    EXPECT_EQ(v[0].cmd, "[\"-g\",\"daemon off;\"]");   // survives round-trip

    EXPECT_EQ(v[1].name, "db");
    EXPECT_EQ(v[1].net, "host");
    EXPECT_EQ(v[1].state, "stopped");
}

TEST(Roster, EmptyArrayIsValid) {
    std::vector<RosterEntry> v;
    EXPECT_TRUE(parse_roster("[]", v));
    EXPECT_TRUE(v.empty());
    EXPECT_TRUE(parse_roster("", v));                  // schema default before any publish
    EXPECT_TRUE(v.empty());
}

TEST(Roster, MalformedRejected) {
    std::vector<RosterEntry> v;
    EXPECT_FALSE(parse_roster("{not json", v));
    EXPECT_FALSE(parse_roster("{\"name\":\"x\"}", v)); // object, not an array
}

TEST(Roster, SkipsEntriesWithoutName) {
    std::vector<RosterEntry> v;
    ASSERT_TRUE(parse_roster("[{\"image\":\"x\"},{\"name\":\"ok\"}]", v));
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "ok");
}
