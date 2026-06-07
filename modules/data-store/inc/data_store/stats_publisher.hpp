#ifndef __data_store_stats_publisher_hpp__
#define __data_store_stats_publisher_hpp__

/// Reusable per-daemon CPU / memory / fd / thread telemetry publisher.
/// Sibling to LogBuffer.  Design: log/L22/design.md.
///
/// Each daemon constructs one instance with a ds key prefix
/// ("services.cloud.iot.cloudd") and a sink that writes the sampled
/// key/value batch, then calls open().  An ACE reactor timer — scheduled
/// on ACE_Reactor::instance() and driven by an ACE_Task active-object
/// thread — fires every STATS_FLUSH_SEC seconds and publishes:
///
///   <prefix>.cpu.permille   parts-per-1000 of one host-second (123 = 12.3%)
///   <prefix>.mem.rss.kb     resident memory, KB
///   <prefix>.fd.count       open file descriptors
///   <prefix>.threads        live task / thread count
///
/// and bumps services.stats.version so the cloud UI long-polls one key.
///
/// CPU / memory / threads come from the process's own container cgroup
/// (/sys/fs/cgroup — v2 with v1 fallback); the fd count from /proc/self/fd.
/// One daemon per container, so these are effectively per-container totals.
///
/// Usage (one instance per daemon, after Client::connect):
///
///   data_store::StatsPublisher g_stats(
///       "services.cloud.iot.cloudd",
///       [&ds](const std::vector<data_store::KV>& kv){ ds.set(kv); });
///   g_stats.open();      // spawns the ACE timer thread
///   ...                  // daemon runs its normal recv_event loop
///   g_stats.close();     // stop timer + join thread (also in dtor)

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"   // KV, Value

namespace data_store {

/// Detected cgroup hierarchy. Probed once at construction.
enum class CgVersion { v2, v1, none };

/// Whole-process resource snapshot. int32 to match the ds integer type;
/// unreadable metrics report 0 (never negative).
struct StatsSample {
    std::int32_t cpu_permille = 0;   // parts-per-1000 of one host-second
    std::int32_t cpu_count    = 0;   // cores the % is normalised against
    std::int32_t mem_rss_kb   = 0;
    std::int32_t fd_count     = 0;
    std::int32_t threads      = 0;
};

/// Test seam: cgroup + proc mount roots. Production defaults.
struct StatsRoots {
    std::string cgroup_root = "/sys/fs/cgroup";
    std::string proc_self   = "/proc/self";
};

/// Writes the sampled KV batch (4 metrics + services.stats.version) to
/// wherever they belong. A Client-backed daemon passes
/// `[&ds](const std::vector<KV>& kv){ ds.set(kv); }`; ds-server passes a
/// lambda over its in-process DataStore; tests capture into a vector.
using StatsSink = std::function<void(const std::vector<KV>&)>;

class StatsPublisher : public ACE_Event_Handler {
public:
    static constexpr int STATS_FLUSH_SEC = 10;

    StatsPublisher(std::string prefix, StatsSink sink,
                   StatsRoots roots = StatsRoots{});
    ~StatsPublisher() override;

    StatsPublisher(const StatsPublisher&)            = delete;
    StatsPublisher& operator=(const StatsPublisher&) = delete;

    /// Schedule the recurring timer on ACE_Reactor::instance() and spawn
    /// one ACE_Task thread to run that singleton reactor's event loop.
    /// @param interval_sec        flush cadence (default STATS_FLUSH_SEC).
    /// @param run_reactor_thread  true (default) spawns one ACE_Task thread
    ///        to run the singleton reactor — for daemons that block elsewhere
    ///        (e.g. Client::recv_event) and never pump the reactor. Pass
    ///        false for daemons that already run the singleton reactor in
    ///        their main loop (iot-httpd, ds-server); the timer then fires on
    ///        their existing reactor thread.
    int  open(int interval_sec = STATS_FLUSH_SEC,
              bool run_reactor_thread = true);

    /// Cancel the timer; if we spawned the reactor thread, end the loop and
    /// join it. Idempotent.
    void close();

    /// Read cgroup + /proc/self/fd and compute CPU% since the previous
    /// sample. The FIRST call records the CPU baseline (cpu=0). Pure of the
    /// sink — unit-tested directly with fixture roots.
    StatsSample sample();

    /// ACE timer callback — sample() then publish().
    int handle_timeout(const ACE_Time_Value&, const void*) override;

private:
    void publish(const StatsSample&);

    struct Impl;                          // ACE_Task active-object thread
    std::unique_ptr<Impl> m_impl;
    std::string           m_prefix;
    StatsSink             m_sink;
    StatsRoots            m_roots;
    CgVersion             m_cg = CgVersion::none;   // probed once in ctor
    long                  m_timer_id = -1;
    bool                  m_own_thread = false;     // we run the reactor loop
    unsigned long long    m_last_usage_usec = 0;
    std::chrono::steady_clock::time_point m_last_t{};
    bool                  m_have_baseline = false;
};

/// Pure parsing / math, exposed for unit tests (no reactor, no sink).
namespace stats_detail {
    CgVersion    detect_cgroup(const StatsRoots&);
    /// usage_usec for the cgroup. Returns false if unavailable.
    bool         read_cpu_usec(const StatsRoots&, CgVersion,
                               unsigned long long& out_usec);
    std::int32_t read_mem_kb (const StatsRoots&, CgVersion);
    std::int32_t read_pids   (const StatsRoots&, CgVersion);
    std::int32_t count_fds   (const StatsRoots&);
    /// Cores available: cgroup v2 cpu.max quota if set, else online CPUs.
    std::int32_t read_ncpu   (const StatsRoots&, CgVersion);
    /// permille of one host-second of CPU over [prev,now] across dt_sec.
    /// Clamps to 0 on counter reset / non-positive dt.
    std::int32_t cpu_permille(unsigned long long prev_usec,
                              unsigned long long now_usec, double dt_sec);
}  // namespace stats_detail

}  // namespace data_store

#endif  // __data_store_stats_publisher_hpp__
