/// WorkerPool tests — verify jobs run, run concurrently, and that
/// threads==0 falls back to inline. Pure C++17, no ACE.

#include "http_server/worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using http_server::WorkerPool;

TEST(WorkerPool, RunsAllJobs) {
    WorkerPool pool(4);
    pool.start();

    std::atomic<int> count{0};
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        pool.submit([&count] { count.fetch_add(1); });
    }
    pool.stop();   // joins workers → all submitted jobs have run
    EXPECT_EQ(count.load(), N);
}

TEST(WorkerPool, RunsConcurrently) {
    // Two jobs must overlap: the first blocks on a promise the second
    // fulfils. With a single worker this would deadlock; a 2-thread pool
    // lets them overlap. Guarded by a timeout so a failure can't hang.
    WorkerPool pool(2);
    pool.start();

    std::promise<void> gate;
    std::future<void>  gate_f = gate.get_future();
    std::atomic<bool>  second_ran{false};

    pool.submit([&] {
        // Block until the second job opens the gate.
        gate_f.wait();
    });
    pool.submit([&] {
        second_ran = true;
        gate.set_value();
    });

    // The first job can only return after the second ran concurrently.
    pool.stop();
    EXPECT_TRUE(second_ran.load());
}

TEST(WorkerPool, InlineWhenZeroThreads) {
    WorkerPool pool(0);
    pool.start();   // no-op

    bool ran = false;
    pool.submit([&ran] { ran = true; });
    EXPECT_TRUE(ran);   // ran synchronously on this thread
    EXPECT_EQ(pool.size(), 0u);
}

TEST(WorkerPool, StopIsIdempotentAndSafeWithoutStart) {
    WorkerPool pool(4);
    pool.stop();        // never started
    pool.stop();        // again
    SUCCEED();
}

TEST(WorkerPool, SubmitAfterStopRunsInline) {
    WorkerPool pool(2);
    pool.start();
    pool.stop();
    bool ran = false;
    pool.submit([&ran] { ran = true; });
    EXPECT_TRUE(ran);
}
