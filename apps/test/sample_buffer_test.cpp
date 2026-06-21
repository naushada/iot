#include <gtest/gtest.h>

#include "lwm2m_sample_buffer.hpp"

namespace tp = ::lwm2m::telemetry;

namespace {
tp::Sample mk(double t, double v) {
    tp::Sample s;
    s.timeUnix = t;
    s.values = { {"10", v} };
    return s;
}
} // namespace

TEST(SampleBuffer, push_and_take_is_fifo_oldest_first) {
    tp::SampleBuffer buf(10);
    for (int k = 0; k < 5; ++k) buf.push(mk(1000 + k, 60 + k));
    EXPECT_EQ(5u, buf.size());
    EXPECT_FALSE(buf.empty());

    auto batch = buf.take(3);
    ASSERT_EQ(3u, batch.size());
    EXPECT_EQ(1000.0, batch[0].timeUnix);   // oldest first
    EXPECT_EQ(1002.0, batch[2].timeUnix);
    EXPECT_EQ(2u, buf.size());              // 2 left
}

TEST(SampleBuffer, take_more_than_present_returns_all) {
    tp::SampleBuffer buf(10);
    buf.push(mk(1, 1));
    buf.push(mk(2, 2));
    auto batch = buf.take(100);
    EXPECT_EQ(2u, batch.size());
    EXPECT_TRUE(buf.empty());
    EXPECT_TRUE(buf.take(5).empty());       // empty buffer → empty batch
}

TEST(SampleBuffer, overflow_evicts_oldest_and_counts_dropped) {
    tp::SampleBuffer buf(3);
    for (int k = 0; k < 5; ++k) buf.push(mk(1000 + k, k));  // 0,1,2 then evict for 3,4
    EXPECT_EQ(3u, buf.size());
    EXPECT_EQ(2u, buf.dropped());
    auto batch = buf.take(3);
    EXPECT_EQ(1002.0, batch[0].timeUnix);   // 1000,1001 evicted; 1002 is oldest kept
    EXPECT_EQ(1004.0, batch[2].timeUnix);
}

TEST(SampleBuffer, requeue_restores_order_and_retries_first) {
    tp::SampleBuffer buf(10);
    for (int k = 0; k < 4; ++k) buf.push(mk(1000 + k, k));
    auto batch = buf.take(2);               // [1000, 1001]
    EXPECT_EQ(2u, buf.size());              // [1002, 1003] remain
    buf.requeue(std::move(batch));          // failed Send → back to front
    EXPECT_EQ(4u, buf.size());
    auto again = buf.take(4);
    ASSERT_EQ(4u, again.size());
    EXPECT_EQ(1000.0, again[0].timeUnix);   // requeued batch retries first
    EXPECT_EQ(1001.0, again[1].timeUnix);
    EXPECT_EQ(1002.0, again[2].timeUnix);
    EXPECT_EQ(1003.0, again[3].timeUnix);
}

TEST(SampleBuffer, requeue_over_capacity_drops_oldest) {
    tp::SampleBuffer buf(3);
    buf.push(mk(1, 1));
    buf.push(mk(2, 2));
    auto batch = buf.take(2);               // hold [1,2]; buffer empty
    buf.push(mk(3, 3));
    buf.push(mk(4, 4));
    buf.push(mk(5, 5));                      // buffer full [3,4,5]
    buf.requeue(std::move(batch));          // no room → both requeued samples dropped
    EXPECT_EQ(3u, buf.size());
    EXPECT_EQ(2u, buf.dropped());
    auto out = buf.take(3);
    EXPECT_EQ(3.0, out[0].timeUnix);        // newest-wins: 3,4,5 kept
}

TEST(SampleBuffer, capacity_zero_clamps_to_one) {
    tp::SampleBuffer buf(0);
    buf.push(mk(1, 1));
    buf.push(mk(2, 2));
    EXPECT_EQ(1u, buf.size());
    EXPECT_EQ(2.0, buf.take(1)[0].timeUnix);
}
