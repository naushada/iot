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
#include <sstream>
#include <string>
#include <vector>

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

// ── per-PID (process mode) ───────────────────────────────────────────
bool read_proc_stat(const std::string& proc_dir,
                    unsigned long long& out_jiffies,
                    std::int32_t& out_threads) {
    std::ifstream ifs(proc_dir + "/stat");
    if (!ifs.is_open()) return false;
    std::string line;
    std::getline(ifs, line);
    // comm (field 2) is parenthesised and may contain spaces/parens — parse
    // the fixed fields *after* the final ')'.
    auto rp = line.rfind(')');
    if (rp == std::string::npos) return false;
    std::istringstream rest(line.substr(rp + 1));
    std::vector<std::string> tok;
    for (std::string t; rest >> t;) tok.push_back(t);
    // After ')': field 3 = tok[0]. utime=14→tok[11], stime=15→tok[12],
    // num_threads=20→tok[17].
    if (tok.size() < 18) return false;
    unsigned long long utime = std::strtoull(tok[11].c_str(), nullptr, 10);
    unsigned long long stime = std::strtoull(tok[12].c_str(), nullptr, 10);
    out_jiffies = utime + stime;
    out_threads = clamp_i32(std::strtoull(tok[17].c_str(), nullptr, 10));
    return true;
}

std::int32_t read_proc_mem_kb(const std::string& proc_dir) {
    std::ifstream ifs(proc_dir + "/statm");
    if (!ifs.is_open()) return 0;
    unsigned long long total = 0, resident = 0;
    if (!(ifs >> total >> resident)) return 0;
    long pg = ::sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    return clamp_i32(resident * (static_cast<unsigned long long>(pg) / 1024ULL));
}

std::int32_t count_fds_in(const std::string& proc_dir) {
    DIR* d = ::opendir((proc_dir + "/fd").c_str());
    if (!d) return 0;
    std::int32_t n = 0;
    while (struct dirent* e = ::readdir(d)) {
        const char* nm = e->d_name;
        if (nm[0] == '.' &&
            (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
        ++n;
    }
    ::closedir(d);
    return n;
}

std::int32_t cpu_permille_jiffies(unsigned long long prev_j,
                                  unsigned long long now_j, double dt_sec) {
    if (dt_sec <= 0.0 || now_j < prev_j) return 0;
    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    double busy_sec = static_cast<double>(now_j - prev_j) / static_cast<double>(hz);
    double permille = (busy_sec / dt_sec) * 1000.0;
    if (permille < 0.0) permille = 0.0;
    return static_cast<std::int32_t>(std::llround(permille));
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

// ── ACE active object: runs a reactor's event loop on its own thread ─
struct StatsPublisher::Impl : public ACE_Task<ACE_MT_SYNCH> {
    std::atomic<bool> running{false};
    ACE_Reactor*      reactor = nullptr;   // the reactor this thread drives

    int svc() override {
        // Take ownership on this thread, then pump until end_reactor_event_loop.
        reactor->owner(ACE_OS::thr_self());
        reactor->run_reactor_event_loop();
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

StatsPublisher::StatsPublisher(std::string prefix, StatsSink sink,
                               StatsPidFn pid_fn, StatsRoots roots)
    : m_impl(std::make_unique<Impl>()),
      m_prefix(std::move(prefix)),
      m_sink(std::move(sink)),
      m_roots(std::move(roots)),
      m_pid_fn(std::move(pid_fn)) {
    // Per-pid mode — no cgroup probe; sample /proc/<pid> in sample().
}

StatsPublisher::~StatsPublisher() { close(); }

int StatsPublisher::open(int interval_sec, bool run_reactor_thread) {
    if (!m_impl || m_timer_id != -1) return 0;     // already scheduled
    if (interval_sec < 1) interval_sec = 1;

    // run_reactor_thread → a PRIVATE reactor we drive on our own thread, so
    // the timer never depends on which thread owns the singleton. Otherwise
    // schedule on the singleton and let the caller's loop dispatch it.
    if (run_reactor_thread) {
        m_priv_reactor = new ACE_Reactor();
        m_reactor = m_priv_reactor;
    } else {
        m_reactor = ACE_Reactor::instance();
    }

    ACE_Time_Value iv(interval_sec);
    long id = m_reactor->schedule_timer(this, nullptr, iv, iv);
    if (id == -1) {
        delete m_priv_reactor; m_priv_reactor = nullptr; m_reactor = nullptr;
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D stats:thread:%t %M %N:%l schedule_timer "
                                   "failed for %C\n"),
                          m_prefix.c_str()),
                         -1);
    }
    m_timer_id = id;

    if (run_reactor_thread) {
        m_impl->reactor = m_reactor;
        m_impl->running.store(true);
        if (m_impl->activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
            m_reactor->cancel_timer(this);
            m_timer_id = -1;
            m_impl->running.store(false);
            delete m_priv_reactor; m_priv_reactor = nullptr; m_reactor = nullptr;
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
    if (m_timer_id != -1 && m_reactor) {
        m_reactor->cancel_timer(this);
        m_timer_id = -1;
    }
    if (m_own_thread && m_impl->running.load()) {
        // Stop our private reactor loop and join the thread.
        m_reactor->end_reactor_event_loop();
        m_impl->wait();              // join the active-object thread
        m_impl->running.store(false);
        m_own_thread = false;
    }
    if (m_priv_reactor) {
        delete m_priv_reactor;
        m_priv_reactor = nullptr;
    }
    m_reactor = nullptr;
}

StatsSample StatsPublisher::sample() {
    // ── per-pid mode: sample /proc/<pid> for a managed child ─────────
    if (m_pid_fn) {
        StatsSample s;
        const long pid = m_pid_fn();
        if (pid <= 0) {                 // not running → all zero (UI shows "—")
            m_have_baseline = false;
            m_last_pid = -1;
            return s;
        }
        const std::string proc_dir =
            m_roots.proc_root + "/" + std::to_string(pid);
        s.mem_rss_kb = stats_detail::read_proc_mem_kb(proc_dir);
        s.fd_count   = stats_detail::count_fds_in(proc_dir);
        long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
        s.cpu_count = static_cast<std::int32_t>(ncpu < 1 ? 1 : ncpu);

        unsigned long long jiffies = 0;
        std::int32_t threads = 0;
        const bool ok = stats_detail::read_proc_stat(proc_dir, jiffies, threads);
        const auto now = std::chrono::steady_clock::now();
        if (ok) {
            s.threads = threads;
            if (m_have_baseline && pid == m_last_pid) {
                const double dt =
                    std::chrono::duration<double>(now - m_last_t).count();
                s.cpu_permille =
                    stats_detail::cpu_permille_jiffies(m_last_usage_usec, jiffies, dt);
            } else {
                m_have_baseline = true;   // first sample / new pid → baseline
            }
            m_last_usage_usec = jiffies;
            m_last_t = now;
            m_last_pid = pid;
        }
        return s;
    }

    StatsSample s;
    s.mem_rss_kb = stats_detail::read_mem_kb(m_roots, m_cg);
    s.threads    = stats_detail::read_pids(m_roots, m_cg);
    // Fall back to the daemon's own /proc/self when the cgroup memory/pids
    // controllers aren't accounted for this unit. cgroups aren't a container
    // thing — systemd puts every service in its own cgroup on bare metal too —
    // but CPU is accounted by default while memory+pids often are not, and on
    // Raspberry Pi the kernel memory cgroup is disabled unless cgroup_enable=
    // memory is on the boot cmdline. The cgroup figure would also fold in child
    // procs (e.g. wifi-client's wpa_supplicant), so this self-only number is a
    // slight under-count — but far better than reporting 0.
    if (s.mem_rss_kb == 0)
        s.mem_rss_kb = stats_detail::read_proc_mem_kb(m_roots.proc_self);
    if (s.threads == 0) {
        unsigned long long j = 0; std::int32_t th = 0;
        if (stats_detail::read_proc_stat(m_roots.proc_self, j, th))
            s.threads = th;
    }
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
