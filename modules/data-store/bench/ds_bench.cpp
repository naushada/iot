/// ds_bench — throughput + latency + memory benchmark for the data-store.
///
/// Drives a running ds-server over the AF_UNIX socket via the public
/// data_store::Client API and reports ops/sec + p50/p95/p99 latency for the hot
/// paths (persistent/volatile set, get, batched set, watch→notify), plus the
/// server's resident memory (RSS) when its pid is given. Writes a JSON summary
/// to the ds key `ds.bench.summary` so the result is queryable / UI-surfaceable.
///
/// Usage: ds-bench [socket=/run/iot/data_store.sock] [n=20000] [vsize=64]
///                 [batch=50] [pid=<ds-server-pid>]
///   Build with -DBUILD_DS_BENCH=ON (links libdatastore_client.a).

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

using bclock = std::chrono::steady_clock;
using us = std::chrono::microseconds;

namespace {

std::string g_summary;   // JSON fragments, joined into ds.bench.summary

std::string arg(int argc, char** argv, const char* key, const std::string& dflt) {
    const std::size_t kl = std::strlen(key);
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], key, kl) == 0 && argv[i][kl] == '=')
            return std::string(argv[i] + kl + 1);
    return dflt;
}

double pct(const std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

/// Read a /proc/<pid>/status field (kB). Returns 0 when unavailable.
long proc_kb(long pid, const char* field) {
    if (pid <= 0) return 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(field, 0) == 0) {
            long kb = 0; std::sscanf(line.c_str() + std::strlen(field), " %ld", &kb);
            return kb;
        }
    return 0;
}

void report(const char* name, const char* jkey, std::vector<double>& lat_us,
            double wall_s, long ops, bool throughput) {
    std::sort(lat_us.begin(), lat_us.end());
    double sum = 0; for (double x : lat_us) sum += x;
    double opsps = (throughput && wall_s > 0) ? ops / wall_s : 0;
    std::printf("%-18s %9.0f ops/s | mean %7.1f  p50 %7.1f  p95 %7.1f  "
                "p99 %7.1f  max %8.1f us\n",
                name, opsps, lat_us.empty() ? 0 : sum / lat_us.size(),
                pct(lat_us, 50), pct(lat_us, 95), pct(lat_us, 99),
                lat_us.empty() ? 0 : lat_us.back());
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s\"%s\":{\"ops\":%.0f,\"p50\":%.0f,\"p95\":%.0f,\"p99\":%.0f}",
                  g_summary.empty() ? "" : ",", jkey, opsps,
                  pct(lat_us, 50), pct(lat_us, 95), pct(lat_us, 99));
    g_summary += buf;
}

} // namespace

int main(int argc, char** argv) {
    const std::string sock = arg(argc, argv, "socket", "/run/iot/data_store.sock");
    const long    N      = std::stol(arg(argc, argv, "n", "20000"));
    const std::size_t VS = std::stoul(arg(argc, argv, "vsize", "64"));
    const long    BATCH  = std::stol(arg(argc, argv, "batch", "50"));
    const long    PID    = std::stol(arg(argc, argv, "pid", "0"));
    const std::string val(VS, 'x');

    data_store::Client c;
    if (auto s = c.connect(sock); !s.ok) {
        std::fprintf(stderr, "connect(%s) failed: %s\n", sock.c_str(), s.err.c_str());
        return 1;
    }
    std::printf("ds-bench: socket=%s n=%ld vsize=%zu batch=%ld pid=%ld\n\n",
                sock.c_str(), N, VS, BATCH, PID);

    const long rss0 = proc_kb(PID, "VmRSS:");

    for (int i = 0; i < 200; ++i) c.set("bench.warm", data_store::Value{val});

    {   // persistent set (write-through + fsync per call)
        std::vector<double> lat; lat.reserve(N);
        auto t0 = bclock::now();
        for (long i = 0; i < N; ++i) { auto a = bclock::now();
            c.set("bench.k" + std::to_string(i % 1000), data_store::Value{val});
            lat.push_back(std::chrono::duration_cast<us>(bclock::now() - a).count()); }
        report("set (persist)", "set_persist", lat,
               std::chrono::duration<double>(bclock::now() - t0).count(), N, true);
    }
    {   // volatile set (in-memory overlay, no fsync)
        std::vector<double> lat; lat.reserve(N);
        auto t0 = bclock::now();
        for (long i = 0; i < N; ++i) { auto a = bclock::now();
            c.set_volatile("bench.v" + std::to_string(i % 1000), data_store::Value{val});
            lat.push_back(std::chrono::duration_cast<us>(bclock::now() - a).count()); }
        report("set (volatile)", "set_volatile", lat,
               std::chrono::duration<double>(bclock::now() - t0).count(), N, true);
    }
    {   // get
        std::vector<double> lat; lat.reserve(N);
        std::vector<data_store::Client::GetResult> out;
        auto t0 = bclock::now();
        for (long i = 0; i < N; ++i) { auto a = bclock::now();
            c.get({"bench.k" + std::to_string(i % 1000)}, out);
            lat.push_back(std::chrono::duration_cast<us>(bclock::now() - a).count()); }
        report("get", "get", lat,
               std::chrono::duration<double>(bclock::now() - t0).count(), N, true);
    }
    {   // batched persistent set (BATCH keys / call) — amortised fsync
        const long iters = N / BATCH;
        std::vector<double> lat; lat.reserve(iters);
        auto t0 = bclock::now();
        for (long i = 0; i < iters; ++i) {
            std::vector<data_store::KV> kv; kv.reserve(BATCH);
            for (long j = 0; j < BATCH; ++j)
                kv.emplace_back("bench.b" + std::to_string(j), data_store::Value{val});
            auto a = bclock::now(); c.set(kv);
            lat.push_back(std::chrono::duration_cast<us>(bclock::now() - a).count()); }
        report("set (batch)", "set_batch", lat,
               std::chrono::duration<double>(bclock::now() - t0).count(), iters * BATCH, true);
    }
    {   // watch -> notify delivery latency
        data_store::Client w;
        if (w.connect(sock).ok) {
            std::mutex m; std::condition_variable cv;
            bclock::time_point recv; std::atomic<bool> got{false};
            w.watch("bench.notify", [&](const data_store::Client::Event&) {
                recv = bclock::now();
                { std::lock_guard<std::mutex> lk(m); got = true; } cv.notify_one(); });
            const long M = std::min<long>(N, 5000);
            std::vector<double> lat; lat.reserve(M);
            for (long i = 0; i < M; ++i) {
                got = false; auto a = bclock::now();
                c.set("bench.notify", data_store::Value{std::to_string(i)});
                std::unique_lock<std::mutex> lk(m);
                if (cv.wait_for(lk, std::chrono::seconds(2), [&]{ return got.load(); }))
                    lat.push_back(std::chrono::duration_cast<us>(recv - a).count()); }
            report("watch->notify", "notify", lat, 1.0, lat.size(), false);
        }
    }

    // Memory: ds-server resident set after the run + its peak (VmHWM).
    const long rss1 = proc_kb(PID, "VmRSS:");
    const long hwm  = proc_kb(PID, "VmHWM:");
    if (PID > 0) {
        std::printf("%-18s rss %ld KB -> %ld KB (delta %+ld) | peak(VmHWM) %ld KB\n",
                    "ds-server mem", rss0, rss1, rss1 - rss0, hwm);
    }

    // Write the summary into ds for at-a-glance reporting / UI.
    char cfg[160];
    std::snprintf(cfg, sizeof(cfg),
                  "{\"config\":{\"n\":%ld,\"vsize\":%zu,\"batch\":%ld},"
                  "\"mem\":{\"rss_kb\":%ld,\"hwm_kb\":%ld},",
                  N, VS, BATCH, rss1, hwm);
    std::string json = std::string(cfg) + g_summary + "}";
    if (auto s = c.set("ds.bench.summary", data_store::Value{json}); s.ok)
        std::printf("\nwrote ds.bench.summary (%zu bytes)\n", json.size());
    else
        std::printf("\nds.bench.summary set failed: %s\n", s.err.c_str());

    c.close();
    return 0;
}
