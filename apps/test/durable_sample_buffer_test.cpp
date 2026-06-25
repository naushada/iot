#include <gtest/gtest.h>

#include <cstdio>    // std::remove
#include <memory>
#include <string>

#include "lwm2m_durable_sample_buffer.hpp"

namespace tp = ::lwm2m::telemetry;

namespace {

tp::Sample mk(double t, double v) {
    tp::Sample s;
    s.timeUnix = t;
    s.values = { {"10", v} };
    return s;
}

// Fixture: one temp DB path, wiped (incl. WAL/SHM sidecars) before + after each
// test. gtest runs cases serially, so reusing one path is safe.
class DurableBuf : public ::testing::Test {
protected:
    const std::string path = "/tmp/iot_dsb_test.db";
    void wipe() {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
    void SetUp() override { wipe(); }
    void TearDown() override { wipe(); }

    std::unique_ptr<tp::DurableSampleBuffer> open(std::size_t cap = 10,
                                                  std::int64_t ttl = 0) {
        return std::make_unique<tp::DurableSampleBuffer>(path, cap, ttl);
    }
};

// ── Contract parity with the in-RAM SampleBuffer (mirrors sample_buffer_test) ──

TEST_F(DurableBuf, push_and_take_is_fifo_oldest_first) {
    auto buf = open();
    for (int k = 0; k < 5; ++k) buf->push(mk(1000 + k, 60 + k));
    EXPECT_EQ(5u, buf->size());
    EXPECT_FALSE(buf->empty());

    auto batch = buf->take(3);
    ASSERT_EQ(3u, batch.size());
    EXPECT_EQ(1000.0, batch[0].timeUnix);          // oldest first
    EXPECT_EQ(1002.0, batch[2].timeUnix);
    ASSERT_EQ(1u, batch[0].values.size());
    EXPECT_EQ("10", batch[0].values[0].first);
    EXPECT_EQ(60.0, batch[0].values[0].second);    // payload round-trips
    EXPECT_EQ(2u, buf->size());                     // 2 still pending (3 leased)
}

TEST_F(DurableBuf, take_more_than_present_returns_all) {
    auto buf = open();
    buf->push(mk(1, 1));
    buf->push(mk(2, 2));
    auto batch = buf->take(100);
    EXPECT_EQ(2u, batch.size());
    EXPECT_EQ(0u, buf->size());
    EXPECT_TRUE(buf->take(5).empty());
}

TEST_F(DurableBuf, overflow_evicts_oldest_and_counts_dropped) {
    auto buf = open(/*cap*/3);
    for (int k = 0; k < 5; ++k) buf->push(mk(1000 + k, k));  // keep 1002,1003,1004
    EXPECT_EQ(3u, buf->size());
    EXPECT_EQ(2u, buf->dropped());
    auto batch = buf->take(3);
    EXPECT_EQ(1002.0, batch[0].timeUnix);
    EXPECT_EQ(1004.0, batch[2].timeUnix);
}

TEST_F(DurableBuf, requeue_restores_order_and_retries_first) {
    auto buf = open();
    for (int k = 0; k < 4; ++k) buf->push(mk(1000 + k, k));
    auto batch = buf->take(2);                       // lease [1000,1001]
    EXPECT_EQ(2u, buf->size());                      // [1002,1003] pending
    buf->requeue(std::move(batch));                  // failed Send → un-lease
    EXPECT_EQ(4u, buf->size());
    auto again = buf->take(4);
    ASSERT_EQ(4u, again.size());
    EXPECT_EQ(1000.0, again[0].timeUnix);            // requeued retries first
    EXPECT_EQ(1003.0, again[3].timeUnix);
}

// ── Durability-specific ──

TEST_F(DurableBuf, survives_reopen) {
    { auto buf = open(); for (int k = 0; k < 5; ++k) buf->push(mk(1000 + k, k)); }
    auto buf = open();                               // reopen same file
    EXPECT_EQ(5u, buf->size());
    auto b = buf->take(5);
    ASSERT_EQ(5u, b.size());
    EXPECT_EQ(1000.0, b[0].timeUnix);                // order preserved on disk
    EXPECT_EQ(1004.0, b[4].timeUnix);
}

TEST_F(DurableBuf, crash_recovery_rearms_leased_uncommitted) {
    { auto buf = open(); for (int k = 0; k < 4; ++k) buf->push(mk(1000 + k, k));
      buf->take(2); /* leased, NOT committed */ }   // simulate crash mid-flight
    auto buf = open();
    EXPECT_EQ(4u, buf->size());                      // leased batch came back
    EXPECT_EQ(4u, buf->take(4).size());
}

TEST_F(DurableBuf, commit_then_reopen_drops_delivered) {
    { auto buf = open(); for (int k = 0; k < 4; ++k) buf->push(mk(1000 + k, k));
      buf->take(2); buf->commit(); }                 // 2 delivered + pruned
    auto buf = open();
    EXPECT_EQ(2u, buf->size());
    auto b = buf->take(2);
    EXPECT_EQ(1002.0, b[0].timeUnix);                // only the undelivered remain
}

TEST_F(DurableBuf, ttl_reaps_old_pending_rows) {
    auto buf = open(/*cap*/100, /*ttl*/100);
    buf->push(mk(/*timeUnix*/1000, 1));              // ts=1000s → old
    buf->push(mk(/*timeUnix*/9999, 2));              // ts=9999s → fresh
    const std::size_t reaped = buf->reap_expired(/*now*/10000);  // cutoff 9900s
    EXPECT_EQ(1u, reaped);
    EXPECT_EQ(1u, buf->size());
    EXPECT_EQ(9999.0, buf->take(1)[0].timeUnix);
}

TEST_F(DurableBuf, factory_falls_back_to_ram_on_empty_or_bad_path) {
    auto ram = tp::make_sample_buffer("", 5, 0);     // empty → in-RAM
    ASSERT_NE(nullptr, ram);
    ram->push(mk(1, 1));
    EXPECT_EQ(1u, ram->size());

    auto bad = tp::make_sample_buffer("/no/such/dir/x.db", 5, 0);  // open fails → in-RAM
    ASSERT_NE(nullptr, bad);
    bad->push(mk(1, 1));
    EXPECT_EQ(1u, bad->size());
}

} // namespace
