#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "lwm2m_object_3_device.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_object_stubs.hpp"

using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::Resource;
using ::lwm2m::ResourceType;
using ::lwm2m::has_op;
namespace objects = ::lwm2m::objects;

namespace {

/// Create a throwaway config tree under /tmp containing deviceObject/0.json
/// with the supplied contents. Returns the configDir (no trailing slash).
std::string write_config(const std::string& json) {
    char dirTemplate[] = "/tmp/iot-l8-XXXXXX";
    char* dir = mkdtemp(dirTemplate);
    std::string configDir(dir);
    std::string subDir = configDir + "/deviceObject";
    ::mkdir(subDir.c_str(), 0755);
    std::ofstream(subDir + "/0.json") << json;
    return configDir;
}

void cleanup(const std::string& dir) {
    std::string f = dir + "/deviceObject/0.json";
    std::remove(f.c_str());
    std::remove((dir + "/deviceObject").c_str());
    std::remove(dir.c_str());
}

} // namespace

/* ─────────────────────────── REQ-OBJ-003 Device ──────────────────────── */

TEST(Objects, REQ_OBJ_003_device_object_present_with_mandatory_rids) {
    ObjectStore store;
    objects::install_device(store, "/no-such-dir");        // forces defaults

    ASSERT_TRUE(store.has(3));
    ASSERT_TRUE(store.has(3, 0));
    for (auto rid : {0u, 1u, 2u, 3u, 4u, 5u, 10u, 13u, 17u, 21u}) {
        EXPECT_TRUE(store.has(3, 0, rid)) << "Missing RID " << rid;
    }

    auto* desc = store.find(3);
    ASSERT_NE(nullptr, desc);
    EXPECT_TRUE(desc->mandatory);
    EXPECT_FALSE(desc->multipleInstance);
}

TEST(Objects, REQ_OBJ_003_reboot_and_factory_reset_are_executable) {
    bool rebooted = false, reset = false;
    ObjectStore store;
    objects::DeviceHooks h;
    h.reboot       = [&](const std::string&) { rebooted = true; return 0; };
    h.factoryReset = [&](const std::string&) { reset    = true; return 0; };
    objects::install_device(store, "/no-such-dir", std::move(h));

    auto* reboot = store.find(3, 0, 4);
    auto* fr     = store.find(3, 0, 5);
    ASSERT_NE(nullptr, reboot);  ASSERT_NE(nullptr, fr);
    EXPECT_TRUE(has_op(reboot->ops, Operations::E));
    EXPECT_TRUE(has_op(fr->ops,     Operations::E));
    ASSERT_TRUE(static_cast<bool>(reboot->execute));
    EXPECT_EQ(0, reboot->execute(""));
    EXPECT_TRUE(rebooted);
    EXPECT_EQ(0, fr->execute(""));
    EXPECT_TRUE(reset);
}

TEST(Objects, REQ_OBJ_003_default_constants_when_json_missing) {
    ObjectStore store;
    objects::install_device(store, "/no-such-dir");
    auto* mfg = store.find(3, 0, 0);
    ASSERT_NE(nullptr, mfg);
    ASSERT_TRUE(mfg->read);
    EXPECT_EQ("Sierra Wireless", mfg->read());
}

TEST(Objects, REQ_OBJ_003_json_override_replaces_default) {
    auto cfg = write_config(R"([
        {"rid": 0, "value": "Acme Corp", "include": true},
        {"rid": 17, "value": "Test Device", "include": true}
    ])");

    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Acme Corp",   store.find(3, 0, 0 )->read());
    EXPECT_EQ("Test Device", store.find(3, 0, 17)->read());
    EXPECT_EQ("LwM2M Client", store.find(3, 0, 1)->read());   // unchanged
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_json_include_false_keeps_default) {
    auto cfg = write_config(R"([
        {"rid": 0, "value": "Should Not Apply", "include": false}
    ])");
    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Sierra Wireless", store.find(3, 0, 0)->read());
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_malformed_json_falls_back_silently) {
    auto cfg = write_config("this is not json");
    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Sierra Wireless", store.find(3, 0, 0)->read());
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_type_mismatch_in_json_falls_back) {
    // Manufacturer must be a string; a numeric override is rejected.
    auto cfg = write_config(R"([{"rid": 0, "value": 42, "include": true}])");
    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Sierra Wireless", store.find(3, 0, 0)->read());
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_live_readers_use_hooks_in_tests) {
    ObjectStore store;
    objects::DeviceHooks h;
    h.memFreeKb  = []() { return std::string("1024"); };
    h.memTotalKb = []() { return std::string("4096"); };
    h.currentTime= []() { return std::string("1700000000"); };
    objects::install_device(store, "/no-such-dir", std::move(h));
    EXPECT_EQ("1024",        store.find(3, 0, 10)->read());
    EXPECT_EQ("4096",        store.find(3, 0, 21)->read());
    EXPECT_EQ("1700000000",  store.find(3, 0, 13)->read());
}

TEST(Objects, REQ_OBJ_003_utc_offset_and_timezone_are_writable) {
    ObjectStore store;
    objects::install_device(store, "/no-such-dir");
    auto* tz = store.find(3, 0, 15);
    ASSERT_TRUE(has_op(tz->ops, Operations::W));
    ASSERT_TRUE(static_cast<bool>(tz->write));
    EXPECT_EQ(0, tz->write("Europe/Berlin"));
    EXPECT_EQ("Europe/Berlin", tz->read());
}

/* ─────────────────────────── REQ-OBJ-005 stubs ───────────────────────── */

TEST(Objects, REQ_OBJ_005_connmon_present_and_readable) {
    ObjectStore store;
    objects::install_connmon(store);
    ASSERT_TRUE(store.has(4, 0));
    auto* netbearer = store.find(4, 0, 0);
    ASSERT_NE(nullptr, netbearer);
    EXPECT_EQ("41", netbearer->read());
    EXPECT_EQ(ResourceType::Integer, netbearer->type);
}

TEST(Objects, REQ_OBJ_005_location_present) {
    ObjectStore store;
    objects::install_location(store);
    ASSERT_TRUE(store.has(6, 0, 0));
    ASSERT_TRUE(store.has(6, 0, 1));
    EXPECT_EQ("0.0", store.find(6, 0, 0)->read());
}

TEST(Objects, REQ_OBJ_005_connstats_present) {
    ObjectStore store;
    objects::install_connstats(store);
    ASSERT_TRUE(store.has(7, 0, 0));
    EXPECT_EQ("0", store.find(7, 0, 2)->read());     // Tx Data
}

TEST(Objects, REQ_OBJ_005_install_canonical_objects_installs_all_four) {
    ObjectStore store;
    int rc = objects::install_canonical_objects(store, "/no-such-dir");
    EXPECT_EQ(0, rc);
    EXPECT_TRUE(store.has(3));
    EXPECT_TRUE(store.has(4));
    EXPECT_TRUE(store.has(6));
    EXPECT_TRUE(store.has(7));
}
