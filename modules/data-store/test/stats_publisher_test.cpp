/// StatsPublisher gtest — cgroup v2/v1 + /proc/self/fd parsing, CPU math,
/// sample() baseline, sink publish, and ACE timer lifecycle. See log/L22.

#include <gtest/gtest.h>

#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "data_store/stats_publisher.hpp"

namespace ds = data_store;
namespace sd = data_store::stats_detail;

namespace {

std::string make_tmpdir() {
    char tmpl[64] = "/tmp/ds-stats-XXXXXX";
    if (char* d = ::mkdtemp(tmpl)) return d;
    return {};
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::trunc);
    ofs << content;
}

void mkdir_p(const std::string& path) { ::mkdir(path.c_str(), 0700); }

// Build a cgroup v2 tree under root; cgroup_root == proc_self == root.
ds::StatsRoots make_cgroup_v2(const std::string& root,
                              unsigned long long usage_usec,
                              unsigned long long mem_bytes,
                              unsigned long long pids) {
    write_file(root + "/cgroup.controllers", "cpu memory pids\n");
    write_file(root + "/cpu.stat",
               "usage_usec " + std::to_string(usage_usec) +
               "\nuser_usec 1\nsystem_usec 1\n");
    write_file(root + "/cpu.max", "max 100000\n");   // unlimited by default
    write_file(root + "/memory.current", std::to_string(mem_bytes) + "\n");
    write_file(root + "/pids.current",   std::to_string(pids) + "\n");
    ds::StatsRoots r;
    r.cgroup_root = root;
    r.proc_self   = root;
    return r;
}

// Build a cgroup v1 tree (no cgroup.controllers → detected as v1).
ds::StatsRoots make_cgroup_v1(const std::string& root,
                              unsigned long long usage_ns,
                              unsigned long long mem_bytes,
                              unsigned long long pids) {
    mkdir_p(root + "/cpuacct");
    mkdir_p(root + "/memory");
    mkdir_p(root + "/pids");
    write_file(root + "/cpuacct/cpuacct.usage", std::to_string(usage_ns) + "\n");
    write_file(root + "/memory/memory.usage_in_bytes",
               std::to_string(mem_bytes) + "\n");
    write_file(root + "/pids/pids.current", std::to_string(pids) + "\n");
    ds::StatsRoots r;
    r.cgroup_root = root;
    r.proc_self   = root;
    return r;
}

// Create root/fd/ with n entries.
void make_fd_dir(const std::string& root, int n) {
    mkdir_p(root + "/fd");
    for (int i = 0; i < n; ++i) write_file(root + "/fd/" + std::to_string(i), "");
}

}  // namespace

// ── detection ──────────────────────────────────────────────────────
TEST(StatsDetect, v2_when_controllers_present) {
    auto r = make_cgroup_v2(make_tmpdir(), 1000000, 1024, 3);
    EXPECT_EQ(sd::detect_cgroup(r), ds::CgVersion::v2);
}

TEST(StatsDetect, v1_when_cpuacct_present) {
    auto r = make_cgroup_v1(make_tmpdir(), 1000000, 1024, 3);
    EXPECT_EQ(sd::detect_cgroup(r), ds::CgVersion::v1);
}

TEST(StatsDetect, none_when_empty) {
    ds::StatsRoots r;
    r.cgroup_root = make_tmpdir();
    EXPECT_EQ(sd::detect_cgroup(r), ds::CgVersion::none);
}

// ── parsers v2 ──────────────────────────────────────────────────────
TEST(StatsParseV2, mem_bytes_to_kb) {
    auto r = make_cgroup_v2(make_tmpdir(), 0, 41943040ULL, 7);  // 40 MiB
    EXPECT_EQ(sd::read_mem_kb(r, ds::CgVersion::v2), 40960);
}

TEST(StatsParseV2, pids) {
    auto r = make_cgroup_v2(make_tmpdir(), 0, 0, 7);
    EXPECT_EQ(sd::read_pids(r, ds::CgVersion::v2), 7);
}

TEST(StatsParseV2, cpu_usec_line) {
    auto r = make_cgroup_v2(make_tmpdir(), 1500000ULL, 0, 0);
    unsigned long long usec = 0;
    ASSERT_TRUE(sd::read_cpu_usec(r, ds::CgVersion::v2, usec));
    EXPECT_EQ(usec, 1500000ULL);
}

// ── parsers v1 ──────────────────────────────────────────────────────
TEST(StatsParseV1, mem_and_pids) {
    auto r = make_cgroup_v1(make_tmpdir(), 0, 41943040ULL, 4);
    EXPECT_EQ(sd::read_mem_kb(r, ds::CgVersion::v1), 40960);
    EXPECT_EQ(sd::read_pids(r, ds::CgVersion::v1), 4);
}

TEST(StatsParseV1, cpu_ns_to_usec) {
    auto r = make_cgroup_v1(make_tmpdir(), 2000000000ULL, 0, 0);  // 2e9ns→2e6us
    unsigned long long usec = 0;
    ASSERT_TRUE(sd::read_cpu_usec(r, ds::CgVersion::v1, usec));
    EXPECT_EQ(usec, 2000000ULL);
}

// ── cpu count ───────────────────────────────────────────────────────
TEST(StatsCpuCount, from_cgroup_v2_quota) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 0, 0, 0);
    write_file(dir + "/cpu.max", "200000 100000\n");   // quota/period = 2
    EXPECT_EQ(sd::read_ncpu(r, ds::CgVersion::v2), 2);
}

TEST(StatsCpuCount, unlimited_falls_back_to_online) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 0, 0, 0);            // cpu.max = "max 100000"
    EXPECT_GE(sd::read_ncpu(r, ds::CgVersion::v2), 1);  // >= 1 online CPU
}

// ── cpu math ────────────────────────────────────────────────────────
TEST(StatsCpu, half_core)   { EXPECT_EQ(sd::cpu_permille(1000000, 1500000, 1.0), 500); }
TEST(StatsCpu, two_cores)   { EXPECT_EQ(sd::cpu_permille(0, 2000000, 1.0), 2000); }
TEST(StatsCpu, reset_zero)  { EXPECT_EQ(sd::cpu_permille(2000000, 1000000, 1.0), 0); }
TEST(StatsCpu, bad_dt_zero) { EXPECT_EQ(sd::cpu_permille(0, 1000000, 0.0), 0); }

// ── fd count ────────────────────────────────────────────────────────
TEST(StatsFds, counts_entries) {
    auto dir = make_tmpdir();
    make_fd_dir(dir, 5);
    ds::StatsRoots r; r.proc_self = dir;
    EXPECT_EQ(sd::count_fds(r), 5);
}

TEST(StatsFds, missing_dir_zero) {
    ds::StatsRoots r; r.proc_self = make_tmpdir();  // no fd/
    EXPECT_EQ(sd::count_fds(r), 0);
}

// ── sample() ────────────────────────────────────────────────────────
TEST(StatsSample, first_call_cpu_zero_others_populated) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 1000000ULL, 41943040ULL, 6);
    make_fd_dir(dir, 9);
    ds::StatsPublisher pub("services.test", nullptr, r);
    auto s = pub.sample();
    EXPECT_EQ(s.cpu_permille, 0);
    EXPECT_EQ(s.mem_rss_kb, 40960);
    EXPECT_EQ(s.threads, 6);
    EXPECT_EQ(s.fd_count, 9);
}

TEST(StatsSample, second_call_reports_cpu) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 1000000ULL, 1024, 1);
    ds::StatsPublisher pub("services.test", nullptr, r);
    (void)pub.sample();                                  // baseline
    write_file(dir + "/cpu.stat", "usage_usec 1500000\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GT(pub.sample().cpu_permille, 0);
}

TEST(StatsSample, none_cgroup_all_zero) {
    ds::StatsRoots r;
    r.cgroup_root = make_tmpdir();
    r.proc_self   = r.cgroup_root;
    ds::StatsPublisher pub("services.test", nullptr, r);
    auto s = pub.sample();
    EXPECT_EQ(s.cpu_permille, 0);
    EXPECT_EQ(s.mem_rss_kb, 0);
    EXPECT_EQ(s.threads, 0);
    EXPECT_EQ(s.fd_count, 0);
}

// ── publish (sink — no server, no reactor) ──────────────────────────
TEST(StatsPublish, sink_receives_four_metrics_plus_version) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 1000000ULL, 41943040ULL, 6);
    make_fd_dir(dir, 9);

    std::vector<ds::KV> got;
    ds::StatsPublisher pub("services.test",
        [&](const std::vector<ds::KV>& kv) { got = kv; }, r);

    pub.handle_timeout(ACE_Time_Value::zero, nullptr);

    ASSERT_EQ(got.size(), 6u);
    bool cpu=false, ncpu=false, mem=false, fd=false, thr=false, ver=false;
    for (auto& kv : got) {
        const std::string& k = kv.first;
        if (k == "services.test.cpu.permille") cpu = true;
        else if (k == "services.test.cpu.count")  { ncpu = true; EXPECT_GE(ds::to_int32(kv.second).value_or(0), 1); }
        else if (k == "services.test.mem.rss.kb") { mem = true; EXPECT_EQ(ds::to_int32(kv.second).value_or(-1), 40960); }
        else if (k == "services.test.fd.count")   { fd  = true; EXPECT_EQ(ds::to_int32(kv.second).value_or(-1), 9); }
        else if (k == "services.test.threads")    { thr = true; EXPECT_EQ(ds::to_int32(kv.second).value_or(-1), 6); }
        else if (k == "services.stats.version")   { ver = true; EXPECT_GT(ds::to_int32(kv.second).value_or(0), 0); }
    }
    EXPECT_TRUE(cpu && ncpu && mem && fd && thr && ver);
}

// ── per-PID (process) mode ───────────────────────────────────────────
TEST(StatsProcMode, samples_self_pid) {
    const long mypid = ::getpid();
    ds::StatsPublisher pub("services.test", nullptr, [&]{ return mypid; });
    auto s = pub.sample();             // baseline → cpu 0, others real
    EXPECT_GE(s.cpu_count, 1);
    EXPECT_GT(s.mem_rss_kb, 0);
    EXPECT_GE(s.fd_count, 1);
    EXPECT_GE(s.threads, 1);
}

TEST(StatsProcMode, pid_not_running_all_zero) {
    ds::StatsPublisher pub("services.test", nullptr, []{ return 0L; });
    auto s = pub.sample();
    EXPECT_EQ(s.cpu_count, 0);
    EXPECT_EQ(s.mem_rss_kb, 0);
    EXPECT_EQ(s.fd_count, 0);
    EXPECT_EQ(s.threads, 0);
}

TEST(StatsProcMode, publish_carries_pid_metrics) {
    const long mypid = ::getpid();
    std::vector<ds::KV> got;
    ds::StatsPublisher pub("services.test",
        [&](const std::vector<ds::KV>& kv) { got = kv; },
        [&]{ return mypid; });
    pub.handle_timeout(ACE_Time_Value::zero, nullptr);
    ASSERT_EQ(got.size(), 6u);
    for (auto& kv : got) {
        if (kv.first == "services.test.cpu.count")
            EXPECT_GE(ds::to_int32(kv.second).value_or(0), 1);
        else if (kv.first == "services.test.mem.rss.kb")
            EXPECT_GT(ds::to_int32(kv.second).value_or(0), 0);
    }
}

// ── timer lifecycle (ACE active object) ─────────────────────────────
TEST(StatsTimer, open_fires_then_close_joins) {
    auto dir = make_tmpdir();
    auto r = make_cgroup_v2(dir, 1000000ULL, 1024, 1);
    make_fd_dir(dir, 2);

    std::atomic<int> fires{0};
    ds::StatsPublisher pub("services.test",
        [&](const std::vector<ds::KV>&) { fires.fetch_add(1); }, r);

    ASSERT_EQ(pub.open(1), 0);                              // 1s interval
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    pub.close();
    EXPECT_GE(fires.load(), 1);

    pub.close();                                            // idempotent

    // Restore the shared singleton reactor for later ds-tests that pump it.
    ACE_Reactor::instance()->reset_reactor_event_loop();
}

TEST(StatsTimer, close_before_open_safe) {
    ds::StatsPublisher pub("services.test", nullptr);
    pub.close();   // must not hang or crash
    SUCCEED();
}
