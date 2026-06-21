#include <gtest/gtest.h>

#include "lwm2m_send_uploader.hpp"
#include "lwm2m_telemetry_pack.hpp"
#include "lwm2m_codec_senml.hpp"
#include "coap_adapter.hpp"

namespace up = ::lwm2m::send;
namespace tp = ::lwm2m::telemetry;
namespace sm = ::lwm2m::senml;

namespace {
tp::Sample mk(double t, double v) {
    tp::Sample s; s.timeUnix = t; s.values = { {"10", v} };
    return s;
}
// Decode a /dp Send frame back to its samples.
std::vector<tp::Sample> samples_of(const std::string& wire) {
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage m;
    coap.parseRequest(wire, m);
    std::vector<sm::Record> recs;
    if (sm::decode_cbor(m.payload, recs) != 0) return {};
    return tp::parse_pack(recs);
}
} // namespace

TEST(Uploader, poll_emits_send_frame_and_marks_in_flight) {
    up::Uploader u("/33000/0/", 100, 10);
    for (int k = 0; k < 3; ++k) u.offer(mk(1000 + k, 60 + k));
    EXPECT_EQ(3u, u.pending());
    EXPECT_FALSE(u.in_flight());

    auto wire = u.poll(0x0042, std::string{0x01});
    ASSERT_FALSE(wire.empty());
    EXPECT_TRUE(u.in_flight());
    EXPECT_EQ(0x0042, u.in_flight_msgid());
    EXPECT_EQ(0u, u.pending());                 // batch taken out of the queue

    auto s = samples_of(wire);
    ASSERT_EQ(3u, s.size());
    EXPECT_EQ(1000.0, s[0].timeUnix);
    EXPECT_EQ(62.0,   s[2].values[0].second);
}

TEST(Uploader, stop_and_wait_no_second_frame_while_in_flight) {
    up::Uploader u("/33000/0/", 100, 10);
    u.offer(mk(1, 1));
    ASSERT_FALSE(u.poll(1, std::string{0x01}).empty());
    u.offer(mk(2, 2));                          // more arrive mid-flight
    EXPECT_TRUE(u.poll(2, std::string{0x02}).empty());   // blocked until ACK
    EXPECT_EQ(1u, u.pending());                 // the new one waits
}

TEST(Uploader, ack_prunes_and_unblocks) {
    up::Uploader u("/33000/0/", 100, 10);
    u.offer(mk(1, 1));
    u.poll(7, std::string{0x01});
    u.on_ack(9);                                // wrong msgid → still in flight
    EXPECT_TRUE(u.in_flight());
    u.on_ack(7);                                // matching → pruned
    EXPECT_FALSE(u.in_flight());
    EXPECT_EQ(0u, u.pending());                 // delivered, gone

    u.offer(mk(2, 2));
    EXPECT_FALSE(u.poll(8, std::string{0x02}).empty());  // can send again
}

TEST(Uploader, timeout_requeues_for_retry) {
    up::Uploader u("/33000/0/", 100, 10);
    u.offer(mk(1000, 1));
    u.offer(mk(1001, 2));
    auto first = u.poll(5, std::string{0x01});
    ASSERT_FALSE(first.empty());
    u.on_timeout();                             // no ACK → requeue
    EXPECT_FALSE(u.in_flight());
    EXPECT_EQ(2u, u.pending());                 // both back in the queue

    auto retry = u.poll(6, std::string{0x02});  // re-sends the same samples
    auto s = samples_of(retry);
    ASSERT_EQ(2u, s.size());
    EXPECT_EQ(1000.0, s[0].timeUnix);
    EXPECT_EQ(1001.0, s[1].timeUnix);
}

TEST(Uploader, maxBatch_limits_frame_size) {
    up::Uploader u("/33000/0/", 100, 2);        // 2 samples per Send
    for (int k = 0; k < 5; ++k) u.offer(mk(1000 + k, k));
    auto wire = u.poll(1, std::string{0x01});
    EXPECT_EQ(2u, samples_of(wire).size());     // only 2 sent
    EXPECT_EQ(3u, u.pending());                 // 3 remain for the next round
}

TEST(Uploader, poll_empty_buffer_is_noop) {
    up::Uploader u("/33000/0/", 100, 10);
    EXPECT_TRUE(u.poll(1, std::string{0x01}).empty());
    EXPECT_FALSE(u.in_flight());
}
