/// Unit tests for the active-object Worker + WorkerPool.

#include <gtest/gtest.h>

#include <memory>

#include "../src/server/data_store.hpp"
#include "../src/server/worker_pool.hpp"

namespace ds = data_store::server;

TEST(WorkerPool, default_size_is_five) {
    auto store = std::make_shared<ds::DataStore>();
    ds::WorkerPool pool(store);
    EXPECT_EQ(5u, pool.size());
}

TEST(WorkerPool, round_robin_visits_every_worker_then_wraps) {
    auto store = std::make_shared<ds::DataStore>();
    ds::WorkerPool pool(store, 3);
    ASSERT_EQ(0, pool.open());

    // 6 picks across a 3-pool: indices 0,1,2,0,1,2.
    ds::Worker* a = pool.next();
    ds::Worker* b = pool.next();
    ds::Worker* c = pool.next();
    ds::Worker* d = pool.next();
    ds::Worker* e = pool.next();
    ds::Worker* f = pool.next();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
    EXPECT_EQ(a, d);
    EXPECT_EQ(b, e);
    EXPECT_EQ(c, f);

    pool.close();
}

TEST(WorkerPool, next_before_open_returns_nullptr_when_pool_size_zero) {
    auto store = std::make_shared<ds::DataStore>();
    ds::WorkerPool pool(store, 0);
    EXPECT_EQ(nullptr, pool.next());
}
