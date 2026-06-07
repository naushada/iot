#include "data_store/stats_publisher.hpp"

#include <ace/Log_Msg.h>
#include <ace/OS_NS_Thread.h>
#include <ace/Reactor.h>
#include <ace/Synch_Traits.h>
#include <ace/Task.h>
#include <ace/Time_Value.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <optional>
#include <string>

namespace data_store {

// ── small /proc + /sys helpers ──────────────────────────────────────
namespace {

bool file_exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

std::optional<unsigned long long> read_uint_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    unsigned long long v = 0;
    if (ifs >> v) return v;
    return std::nullopt;
}

// Clamp an unsigned count into the ds int32 domain.
std::int32_t clamp_i32(unsigned long long v) {
    constexpr unsigned long long kMax =
        static_cast<unsigned long long>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(v > kMax ? kMax : v);
}

}  // namespace

// ── stats_detail: pure parsing / math (unit-tested directly) ─────────
namespace stats_detail {

CgVersion detect_cgroup(const StatsRoots& r) {
    if (file_exists(r.cgroup_root + "/cgroup.controllers")) return CgVersion::v2;
    if (file_exists(r.cgroup_root + "/cpuacct/cpuacct.usage") ||
        file_exists(r.cgroup_root + "/cpu,cpuacct/cpuacct.usage"))
        return CgVersion::v1;
    return CgVersion::none;
}

bool read_cpu_usec(const StatsRoots& r, CgVersion cg,
                   unsigned long long& out_usec) {
    if (cg == CgVersion::v2) {
        std::ifstream ifs(r.cgroup_root + "/cpu.stat");
        if (!ifs.is_open()) return false;
        std::string key;
        unsigned long long val = 0;
        while (ifs >> key >> val) {
            if (key == "usage_usec") { out_usec = val; return true; }
        }
        return false;
    }
    if (cg == CgVersion::v1) {
        // cpuacct.usage is nanoseconds; normalise to microseconds.
        for (const char* sub : {"/cpuacct/cpuacct.usage",
                                "/cpu,cpuacct/cpuacct.usage"}) {
            if (auto ns = read_uint_file(r.cgroup_root + sub)) {
                out_usec = *ns / 1000ULL;
                return true;
            }
        }
        return false;
    }
    return false;
}

std::int32_t read_mem_kb(const StatsRoots& r, CgVersion cg) {
    std::optional<unsigned long long> bytes;
    if (cg == CgVersion::v2)
        bytes = read_uint_file(r.cgroup_root + "/memory.current");
    else if (cg == CgVersion::v1)
        bytes = read_uint_file(r.cgroup_root + "/memory/memory.usage_in_bytes");
    if (!bytes) return 0;
    return clamp_i32(*bytes / 1024ULL);
}

std::int32_t read_pids(const StatsRoots& r, CgVersion cg) {
    std::optional<unsigned long long> n;
    if (cg == CgVersion::v2)
        n = read_uint_file(r.cgroup_root + "/pids.current");
    else if (cg == CgVersion::v1)
        n = read_uint_file(r.cgroup_root + "/pids/pids.current");
    if (!n) return 0;
    return clamp_i32(*n);
}

std::int32_t count_fds(const StatsRoots& r) {
    DIR* d = ::opendir((r.proc_self + "/fd").c_str());
    if (!d) return 0;
    std::int32_t n = 0;
    while (struct dirent* e = ::readdir(d)) {
        const char* nm = e->d_name;
        if (nm[0] == '.' &&
            (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;  // skip "." and ".."
        ++n;
    }
    ::closedir(d);
    return n;
}

std::int32_t read_ncpu(const StatsRoots& r, CgVersion cg) {
    // Prefer the cgroup CPU quota (effective cores the container may use).
    if (cg == CgVersion::v2) {
        std::ifstream ifs(r.cgroup_root + "/cpu.max");
        std::string quota, period;
        if ((ifs >> quota >> period) && quota != "max") {
            char* e1 = nullptr; char* e2 = nullptr;
            long long q = std::strtoll(quota.c_str(),  &e1, 10);
            long long p = std::strtoll(period.c_str(), &e2, 10);
            if (q > 0 && p > 0)
                return clamp_i32(static_cast<unsigned long long>((q + p - 1) / p));
        }
    }
    // Unlimited / v1 / none → online processors.
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return static_cast<std::int32_t>(n < 1 ? 1 : n);
}

std::int32_t cpu_permille(unsigned long long prev_usec,
                          unsigned long long now_usec, double dt_sec) {
    if (dt_sec <= 0.0 || now_usec < prev_usec) return 0;  // reset / bad dt
    double busy_sec  = static_cast<double>(now_usec - prev_usec) / 1e6;
    double cores     = busy_sec / dt_sec;          // fraction of one host core
    double permille  = cores * 1000.0;
    if (permille < 0.0) permille = 0.0;
    return static_cast<std::int32_t>(std::llround(permille));
}

}  // namespace stats_detail

// ── ACE active object: runs the singleton reactor loop ──────────────
struct StatsPublisher::Impl : public ACE_Task<ACE_MT_SYNCH> {
    std::atomic<bool> running{false};

    int svc() override {
        // Own the reactor on this thread, then pump until end_reactor_event_loop.
        ACE_Reactor::instance()->owner(ACE_OS::thr_self());
        ACE_Reactor::instance()->run_reactor_event_loop();
        return 0;
    }
};

// ── StatsPublisher ──────────────────────────────────────────────────
StatsPublisher::StatsPublisher(std::string prefix, StatsSink sink,
                               StatsRoots roots)
    : m_impl(std::make_unique<Impl>()),
      m_prefix(std::move(prefix)),
      m_sink(std::move(sink)),
      m_roots(std::move(roots)) {
    m_cg = stats_detail::detect_cgroup(m_roots);
}

StatsPublisher::~StatsPublisher() { close(); }

int StatsPublisher::open(int interval_sec, bool run_reactor_thread) {
    if (!m_impl || m_timer_id != -1) return 0;     // already scheduled
    if (interval_sec < 1) interval_sec = 1;

    ACE_Time_Value iv(interval_sec);
    long id = ACE_Reactor::instance()->schedule_timer(this, nullptr, iv, iv);
    if (id == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D stats:thread:%t %M %N:%l schedule_timer "
                                   "failed for %C\n"),
                          m_prefix.c_str()),
                         -1);
    }
    m_timer_id = id;

    if (run_reactor_thread) {
        m_impl->running.store(true);
        if (m_impl->activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
            ACE_Reactor::instance()->cancel_timer(this);
            m_timer_id = -1;
            m_impl->running.store(false);
            ACE_ERROR_RETURN((LM_ERROR,
                              ACE_TEXT("%D stats:thread:%t %M %N:%l activate "
                                       "failed for %C errno=%d\n"),
                              m_prefix.c_str(), errno),
                             -1);
        }
        m_own_thread = true;
    }
    return 0;
}

void StatsPublisher::close() {
    if (!m_impl) return;
    if (m_timer_id != -1) {
        ACE_Reactor::instance()->cancel_timer(this);
        m_timer_id = -1;
    }
    if (m_own_thread && m_impl->running.load()) {
        // We own the loop — stop it and join. Daemons that pump the reactor
        // themselves (run_reactor_thread=false) keep owning it.
        ACE_Reactor::instance()->end_reactor_event_loop();
        m_impl->wait();              // join the active-object thread
        m_impl->running.store(false);
        m_own_thread = false;
    }
}

StatsSample StatsPublisher::sample() {
    StatsSample s;
    s.mem_rss_kb = stats_detail::read_mem_kb(m_roots, m_cg);
    s.threads    = stats_detail::read_pids(m_roots, m_cg);
    s.fd_count   = stats_detail::count_fds(m_roots);
    s.cpu_count  = stats_detail::read_ncpu(m_roots, m_cg);

    unsigned long long usage = 0;
    const bool have = stats_detail::read_cpu_usec(m_roots, m_cg, usage);
    const auto now = std::chrono::steady_clock::now();
    if (have) {
        if (m_have_baseline) {
            const double dt =
                std::chrono::duration<double>(now - m_last_t).count();
            s.cpu_permille =
                stats_detail::cpu_permille(m_last_usage_usec, usage, dt);
        } else {
            m_have_baseline = true;     // first sample → baseline only
        }
        m_last_usage_usec = usage;
        m_last_t = now;
    }
    return s;
}

void StatsPublisher::publish(const StatsSample& s) {
    if (!m_sink) return;
    std::vector<KV> kv;
    kv.reserve(6);
    kv.emplace_back(m_prefix + ".cpu.permille", Value{s.cpu_permille});
    kv.emplace_back(m_prefix + ".cpu.count",    Value{s.cpu_count});
    kv.emplace_back(m_prefix + ".mem.rss.kb",   Value{s.mem_rss_kb});
    kv.emplace_back(m_prefix + ".fd.count",     Value{s.fd_count});
    kv.emplace_back(m_prefix + ".threads",      Value{s.threads});
    // Bump services.stats.version so the cloud UI long-poll wakes up.
    const auto ts = static_cast<std::int32_t>(std::time(nullptr));
    kv.emplace_back("services.stats.version", Value{ts});
    m_sink(kv);
}

int StatsPublisher::handle_timeout(const ACE_Time_Value&, const void*) {
    publish(sample());
    return 0;
}

}  // namespace data_store
