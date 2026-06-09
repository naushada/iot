#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "lwm2m_object_3_device.hpp"
#include "lwm2m_object_cert.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_object_stubs.hpp"

using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::Resource;
using ::lwm2m::ResourceType;
using ::lwm2m::has_op;
namespace objects = ::lwm2m::objects;

namespace {

/// Pick a writable parent dir for the throwaway config tree. Minimal
/// container images (e.g. the iot:latest runtime stage) don't always
/// ship a `/tmp`, in which case mkdtemp returns nullptr and the
/// fixture used to crash with "std::string null". Now we try /tmp,
/// auto-create it, and fall back to $PWD. Returns nullptr only when
/// every candidate fails; callers SKIP rather than fail in that case.
char* make_temp_dir(char* dirTemplate, std::size_t cap) {
    auto attempt = [&](const char* tmpl) -> char* {
        std::strncpy(dirTemplate, tmpl, cap - 1);
        dirTemplate[cap - 1] = '\0';
        return mkdtemp(dirTemplate);
    };
    if (char* d = attempt("/tmp/iot-l8-XXXXXX")) return d;
    // /tmp absent or unwritable — create it (mkdir is a no-op if it
    // already exists with permission issues, in which case attempt #3
    // below catches the failure).
    ::mkdir("/tmp", 01777);
    if (char* d = attempt("/tmp/iot-l8-XXXXXX")) return d;
    return attempt("./iot-l8-XXXXXX");
}

/// Create a throwaway config tree containing deviceObject/0.lua with
/// `resourcesBody` plugged into the canonical envelope:
///   return { deviceObject = { instance = 0, resources = { <body> } } }
/// Returns the configDir (no trailing slash), or "" if no writable
/// scratch directory could be created (callers SKIP the test).
std::string write_config(const std::string& resourcesBody) {
    char dirTemplate[64];
    char* dir = make_temp_dir(dirTemplate, sizeof(dirTemplate));
    if (!dir) return {};
    std::string configDir(dir);
    std::string subDir = configDir + "/deviceObject";
    ::mkdir(subDir.c_str(), 0755);
    std::ofstream(subDir + "/0.lua")
        << "return { deviceObject = { instance = 0, resources = {"
        << resourcesBody
        << "} } }\n";
    return configDir;
}

/// Write a raw Lua string verbatim — for the malformed-config test.
/// Same fallback rules as write_config; returns "" when no scratch
/// directory could be created.
std::string write_config_raw(const std::string& body) {
    char dirTemplate[64];
    char* dir = make_temp_dir(dirTemplate, sizeof(dirTemplate));
    if (!dir) return {};
    std::string configDir(dir);
    std::string subDir = configDir + "/deviceObject";
    ::mkdir(subDir.c_str(), 0755);
    std::ofstream(subDir + "/0.lua") << body;
    return configDir;
}

#define SKIP_IF_NO_TMP(cfg) \
    do { if ((cfg).empty()) GTEST_SKIP() \
        << "no writable scratch dir (no /tmp + cwd unwritable)"; } while (0)

void cleanup(const std::string& dir) {
    std::string f = dir + "/deviceObject/0.lua";
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
    auto cfg = write_config(
        "[0]  = { value = 'Acme Corp',   include = true },"
        "[17] = { value = 'Test Device', include = true },");
    SKIP_IF_NO_TMP(cfg);

    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Acme Corp",   store.find(3, 0, 0 )->read());
    EXPECT_EQ("Test Device", store.find(3, 0, 17)->read());
    EXPECT_EQ("LwM2M Client", store.find(3, 0, 1)->read());   // unchanged
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_json_include_false_keeps_default) {
    auto cfg = write_config(
        "[0] = { value = 'Should Not Apply', include = false },");
    SKIP_IF_NO_TMP(cfg);
    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Sierra Wireless", store.find(3, 0, 0)->read());
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_malformed_json_falls_back_silently) {
    auto cfg = write_config_raw("this is not lua");
    SKIP_IF_NO_TMP(cfg);
    ObjectStore store;
    objects::install_device(store, cfg);
    EXPECT_EQ("Sierra Wireless", store.find(3, 0, 0)->read());
    cleanup(cfg);
}

TEST(Objects, REQ_OBJ_003_type_mismatch_in_json_falls_back) {
    // Manufacturer must be a string; a numeric override is rejected
    // (string_or falls through to the default when the value variant
    // doesn't hold std::string).
    auto cfg = write_config("[0] = { value = 42, include = true },");
    SKIP_IF_NO_TMP(cfg);
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
    EXPECT_TRUE(store.has(objects::kCertObjectOid));   // OID 2048 cert object
}

/* ───────────────────── Custom OID 2048 credential object ─────────────── */

namespace {
std::string slurp(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}
} // namespace

TEST(CertObject, registers_three_artifact_instances_with_layout) {
    ObjectStore store;
    objects::install_cert(store, "/no-such-dir");

    auto* desc = store.find(objects::kCertObjectOid);
    ASSERT_NE(nullptr, desc);
    EXPECT_TRUE(desc->multipleInstance);

    // Instance per artifact, RID 0 reports the type.
    ASSERT_TRUE(store.has(2048, 0, 0)); EXPECT_EQ("ca",   store.find(2048,0,0)->read());
    ASSERT_TRUE(store.has(2048, 1, 0)); EXPECT_EQ("cert", store.find(2048,1,0)->read());
    ASSERT_TRUE(store.has(2048, 2, 0)); EXPECT_EQ("key",  store.find(2048,2,0)->read());

    // RID 1 (data) is opaque write-only on every instance.
    for (auto iid : {0u, 1u, 2u}) {
        auto* data = store.find(2048, iid, 1);
        ASSERT_NE(nullptr, data) << "iid " << iid;
        EXPECT_EQ(ResourceType::Opaque, data->type);
        EXPECT_TRUE(has_op(data->ops, Operations::W));
        EXPECT_TRUE(static_cast<bool>(data->write));
    }
    // Apply (RID 3, execute) lives only on instance 0.
    auto* apply = store.find(2048, 0, 3);
    ASSERT_NE(nullptr, apply);
    EXPECT_TRUE(has_op(apply->ops, Operations::E));
    EXPECT_FALSE(store.has(2048, 1, 3));
    EXPECT_FALSE(store.has(2048, 2, 3));
}

TEST(CertObject, write_then_apply_materializes_cert_family) {
    char tmpl[64];
    char* dir = make_temp_dir(tmpl, sizeof(tmpl));
    if (!dir) GTEST_SKIP() << "no writable scratch dir";
    std::string certDir(dir);

    ObjectStore store;
    objects::install_cert(store, certDir);

    // Server WRITEs each artifact's opaque data (staged, not yet on disk).
    EXPECT_EQ(0, store.find(2048, 0, 1)->write("-----CA PEM-----\n"));
    EXPECT_EQ(0, store.find(2048, 1, 1)->write("-----CERT PEM-----\n"));
    EXPECT_EQ(0, store.find(2048, 2, 1)->write("-----KEY PEM-----\n"));
    // Nothing on disk until the apply trigger fires.
    struct stat stbuf;
    EXPECT_NE(0, ::stat((certDir + "/ca.crt").c_str(), &stbuf));

    // EXECUTE /2048/0/3 commits the whole family atomically.
    ASSERT_EQ(0, store.find(2048, 0, 3)->execute(""));

    EXPECT_EQ("-----CA PEM-----\n",   slurp(certDir + "/ca.crt"));
    EXPECT_EQ("-----CERT PEM-----\n", slurp(certDir + "/client.crt"));
    EXPECT_EQ("-----KEY PEM-----\n",  slurp(certDir + "/client.key"));

    // Private key must be 0600; cert/CA world-readable.
    ASSERT_EQ(0, ::stat((certDir + "/client.key").c_str(), &stbuf));
    EXPECT_EQ(0600, stbuf.st_mode & 0777);
    ASSERT_EQ(0, ::stat((certDir + "/client.crt").c_str(), &stbuf));
    EXPECT_EQ(0644, stbuf.st_mode & 0777);

    for (auto* f : {"/ca.crt", "/client.crt", "/client.key"})
        std::remove((certDir + f).c_str());
    std::remove(certDir.c_str());
}

TEST(CertObject, apply_invokes_reload_hook_and_runs_verify) {
    int applied = 0, verified = 0;
    objects::CertHooks h;
    h.store_artifact = [](const std::string&, const std::string&) { return 0; };
    h.apply  = [&]() { ++applied; return 0; };
    h.verify = [&](const std::string&, const std::string& hash,
                   const std::string&) { ++verified; return hash == "good" ? 0 : -1; };

    ObjectStore store;
    // require_complete=false so the single-artifact apply commits (this test
    // exercises the hook + verify path, not the family-completeness gate).
    objects::install_cert(store, "/unused", std::move(h), /*require_complete*/false);

    store.find(2048, 1, 1)->write("cert-bytes");
    store.find(2048, 1, 2)->write("good");          // hash → verify runs, passes
    EXPECT_EQ(0, store.find(2048, 0, 3)->execute(""));
    EXPECT_EQ(1, applied);
    EXPECT_EQ(1, verified);

    // A bad hash aborts the apply (reload hook not re-invoked).
    store.find(2048, 2, 1)->write("key-bytes");
    store.find(2048, 2, 2)->write("bad");
    EXPECT_NE(0, store.find(2048, 0, 3)->execute(""));
    EXPECT_EQ(1, applied);                            // unchanged
}

TEST(CertObject, apply_deferred_until_family_complete_then_self_heals) {
    char tmpl[64];
    char* dir = make_temp_dir(tmpl, sizeof(tmpl));
    if (!dir) GTEST_SKIP() << "no writable scratch dir";
    std::string certDir(dir);

    ObjectStore store;
    objects::install_cert(store, certDir);   // require_complete = true (default)

    // First push lost the key WRITE: only ca + cert staged.
    store.find(2048, 0, 1)->write("CA");
    store.find(2048, 1, 1)->write("CERT");
    EXPECT_NE(0, store.find(2048, 0, 3)->execute(""));   // deferred, not applied
    struct stat st;
    EXPECT_NE(0, ::stat((certDir + "/ca.crt").c_str(), &st));   // nothing written
    EXPECT_EQ("", store.find(2048, 0, 4)->read());             // Applied still empty

    // Re-push fills the missing key; the retained ca+cert + new key now apply.
    store.find(2048, 2, 1)->write("KEY");
    EXPECT_EQ(0, store.find(2048, 0, 3)->execute(""));
    EXPECT_EQ("CA",   slurp(certDir + "/ca.crt"));
    EXPECT_EQ("CERT", slurp(certDir + "/client.crt"));
    EXPECT_EQ("KEY",  slurp(certDir + "/client.key"));

    // RID 4 now reports a stable 16-hex fingerprint of the committed family.
    const std::string fp = store.find(2048, 0, 4)->read();
    EXPECT_EQ(16u, fp.size());
    EXPECT_NE("", fp);

    for (auto* f : {"/ca.crt", "/client.crt", "/client.key"})
        std::remove((certDir + f).c_str());
    std::remove(certDir.c_str());
}

TEST(CertObject, applied_fingerprint_is_deterministic_for_same_family) {
    objects::CertHooks h;
    h.store_artifact = [](const std::string&, const std::string&) { return 0; };
    auto fp_of = [&](const std::string& ca, const std::string& c,
                     const std::string& k) {
        ObjectStore store;
        objects::install_cert(store, "/unused", h);
        store.find(2048, 0, 1)->write(ca);
        store.find(2048, 1, 1)->write(c);
        store.find(2048, 2, 1)->write(k);
        EXPECT_EQ(0, store.find(2048, 0, 3)->execute(""));
        return store.find(2048, 0, 4)->read();
    };
    EXPECT_EQ(fp_of("A", "B", "C"), fp_of("A", "B", "C"));   // stable
    EXPECT_NE(fp_of("A", "B", "C"), fp_of("A", "B", "X"));   // sensitive to key
}
