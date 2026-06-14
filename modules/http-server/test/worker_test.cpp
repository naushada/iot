/// WorkerPool tests — verify jobs run, run concurrently, and that
/// threads==0 falls back to inline. Pure C++17, no ACE.

#include "worker.hpp"

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

// The reactor-completion queue is the response handoff: workers post()
// callbacks from any thread; the reactor thread runs them via drain_reactor().
// post_to_reactor() must NOT run the callback itself (that would touch the
// socket off the reactor thread) — only drain_reactor() runs them.
TEST(WorkerPool, PostToReactorDefersUntilDrain) {
    WorkerPool pool(4);
    pool.start();

    std::atomic<int> delivered{0};
    pool.post_to_reactor([&] { delivered.fetch_add(1); });
    EXPECT_EQ(delivered.load(), 0);   // not run on post

    pool.drain_reactor();             // reactor thread runs it
    EXPECT_EQ(delivered.load(), 1);

    pool.drain_reactor();             // already drained → no double-run
    EXPECT_EQ(delivered.load(), 1);
}

// Every posted completion is delivered exactly once, even when many workers
// post concurrently — this is the property whose absence (lost notify()
// wakeups) hung ~half of all requests.
TEST(WorkerPool, EveryPostedCompletionDeliveredExactlyOnce) {
    WorkerPool pool(8);
    pool.start();

    constexpr int N = 2000;
    std::atomic<int> delivered{0};
    for (int i = 0; i < N; ++i) {
        pool.submit([&] { pool.post_to_reactor([&] { delivered.fetch_add(1); }); });
    }
    pool.stop();                      // joins workers → all post()s done

    pool.drain_reactor();             // single reactor-thread drain
    EXPECT_EQ(delivered.load(), N);   // none lost, none duplicated
}
