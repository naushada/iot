/// ds_bench — throughput + latency benchmark for the data-store client/server.
///
/// Drives a running ds-server over the AF_UNIX socket via the public
/// data_store::Client API and reports ops/sec + p50/p95/p99 latency for the
/// hot paths: persistent set (write-through + fsync), volatile set (no fsync),
/// get, batched set, and watch→notify delivery.
///
/// Usage: ds-bench [socket=/run/iot/data_store.sock] [n=20000] [vsize=64] [batch=50]
///   Build with -DBUILD_DS_BENCH=ON (links libdatastore_client.a).

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using clock_t_ = std::chrono::steady_clock;
using us = std::chrono::microseconds;

namespace {

std::string arg(int argc, char** argv, const char* key, const std::string& dflt) {
    const std::size_t kl = std::strlen(key);
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], key, kl) == 0 && argv[i][kl] == '=')
            return std::string(argv[i] + kl + 1);
    return dflt;
}

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

void report(const char* name, std::vector<double>& lat_us, double wall_s,
            long ops) {
    std::sort(lat_us.begin(), lat_us.end());
    double sum = 0; for (double x : lat_us) sum += x;
    std::printf("%-18s %9.0f ops/s | mean %7.1f  p50 %7.1f  p95 %7.1f  "
                "p99 %7.1f  max %8.1f us\n",
                name, ops / wall_s, lat_us.empty() ? 0 : sum / lat_us.size(),
                pct(lat_us, 50), pct(lat_us, 95), pct(lat_us, 99),
                lat_us.empty() ? 0 : lat_us.back());
}

} // namespace

int main(int argc, char** argv) {
    const std::string sock = arg(argc, argv, "socket", "/run/iot/data_store.sock");
    const long    N      = std::stol(arg(argc, argv, "n", "20000"));
    const std::size_t VS = std::stoul(arg(argc, argv, "vsize", "64"));
    const long    BATCH  = std::stol(arg(argc, argv, "batch", "50"));
    const std::string val(VS, 'x');

    data_store::Client c;
    if (auto s = c.connect(sock); !s.ok) {
        std::fprintf(stderr, "connect(%s) failed: %s\n", sock.c_str(), s.err.c_str());
        return 1;
    }
    std::printf("ds-bench: socket=%s n=%ld vsize=%zu batch=%ld\n\n", sock.c_str(), N, VS, BATCH);

    // Warmup.
    for (int i = 0; i < 200; ++i) c.set("bench.warm", data_store::Value{val});

    // 1. persistent set (write-through + fsync per call).
    {
        std::vector<double> lat; lat.reserve(N);
        auto t0 = clock_t_::now();
        for (long i = 0; i < N; ++i) {
            std::string k = "bench.k" + std::to_string(i % 1000);
            auto a = clock_t_::now();
            c.set(k, data_store::Value{val});
            lat.push_back(std::chrono::duration_cast<us>(clock_t_::now() - a).count());
        }
        report("set (persist)", lat, std::chrono::duration<double>(clock_t_::now() - t0).count(), N);
    }

    // 2. volatile set (in-memory overlay, no fsync).
    {
        std::vector<double> lat; lat.reserve(N);
        auto t0 = clock_t_::now();
        for (long i = 0; i < N; ++i) {
            std::string k = "bench.v" + std::to_string(i % 1000);
            auto a = clock_t_::now();
            c.set_volatile(k, data_store::Value{val});
            lat.push_back(std::chrono::duration_cast<us>(clock_t_::now() - a).count());
        }
        report("set (volatile)", lat, std::chrono::duration<double>(clock_t_::now() - t0).count(), N);
    }

    // 3. get.
    {
        std::vector<double> lat; lat.reserve(N);
        std::vector<data_store::Client::GetResult> out;
        auto t0 = clock_t_::now();
        for (long i = 0; i < N; ++i) {
            std::string k = "bench.k" + std::to_string(i % 1000);
            auto a = clock_t_::now();
            c.get({k}, out);
            lat.push_back(std::chrono::duration_cast<us>(clock_t_::now() - a).count());
        }
        report("get", lat, std::chrono::duration<double>(clock_t_::now() - t0).count(), N);
    }

    // 4. batched persistent set (BATCH keys per call) — amortised fsync.
    {
        const long iters = N / BATCH;
        std::vector<double> lat; lat.reserve(iters);
        auto t0 = clock_t_::now();
        for (long i = 0; i < iters; ++i) {
            std::vector<data_store::KV> kv; kv.reserve(BATCH);
            for (long j = 0; j < BATCH; ++j)
                kv.emplace_back("bench.b" + std::to_string(j), data_store::Value{val});
            auto a = clock_t_::now();
            c.set(kv);
            lat.push_back(std::chrono::duration_cast<us>(clock_t_::now() - a).count());
        }
        report("set (batch)", lat, std::chrono::duration<double>(clock_t_::now() - t0).count(), iters * BATCH);
    }

    // 5. watch -> notify delivery latency (set on one client, callback on another).
    {
        data_store::Client w;
        if (w.connect(sock).ok) {
            std::mutex m; std::condition_variable cv;
            clock_t_::time_point recv; std::atomic<bool> got{false};
            w.watch("bench.notify", [&](const data_store::Client::Event&) {
                recv = clock_t_::now();
                { std::lock_guard<std::mutex> lk(m); got = true; } cv.notify_one();
            });
            const long M = std::min<long>(N, 5000);
            std::vector<double> lat; lat.reserve(M);
            for (long i = 0; i < M; ++i) {
                got = false;
                auto a = clock_t_::now();
                c.set("bench.notify", data_store::Value{std::to_string(i)});
                std::unique_lock<std::mutex> lk(m);
                if (cv.wait_for(lk, std::chrono::seconds(2), [&]{ return got.load(); }))
                    lat.push_back(std::chrono::duration_cast<us>(recv - a).count());
            }
            report("watch->notify", lat, 1.0, lat.size());  // latency-focused
        }
    }

    c.close();
    return 0;
}
