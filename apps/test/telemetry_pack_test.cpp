#include <gtest/gtest.h>

#include "lwm2m_telemetry_pack.hpp"
#include "lwm2m_codec_senml.hpp"

namespace t  = ::lwm2m::telemetry;
namespace sm = ::lwm2m::senml;

/// A 3-sample batch of speed(/10) + rpm(/11) under /33000/0/, 5 s apart.
static std::vector<t::Sample> make_batch() {
    std::vector<t::Sample> b;
    for (int k = 0; k < 3; ++k) {
        t::Sample s;
        s.timeUnix = 1718000000.0 + k * 5.0;
        s.values = { {"10", 60.0 + k}, {"11", 2000.0 + k * 50} };
        b.push_back(s);
    }
    return b;
}

TEST(TelemetryPack, build_sets_bn_bt_and_per_sample_t) {
    auto recs = t::build_pack("/33000/0/", make_batch());
    ASSERT_EQ(6u, recs.size());                       // 3 samples x 2 signals
    // Every record carries the base path + base time of the first sample.
    for (const auto& r : recs) {
        EXPECT_EQ("/33000/0/", r.baseName);
        EXPECT_TRUE(r.hasBaseTime);
        EXPECT_EQ(1718000000.0, r.baseTime);
        EXPECT_TRUE(r.hasTime);
    }
    // Sample 0 → t=0, sample 2 → t=10.
    EXPECT_EQ(0.0,  recs[0].time);
    EXPECT_EQ(10.0, recs[4].time);
    EXPECT_EQ(10.0, recs[5].time);
}

TEST(TelemetryPack, roundtrip_through_parse) {
    auto recs = t::build_pack("/33000/0/", make_batch());
    auto back = t::parse_pack(recs);
    ASSERT_EQ(3u, back.size());
    for (int k = 0; k < 3; ++k) {
        EXPECT_EQ(1718000000.0 + k * 5.0, back[k].timeUnix);   // absolute time restored
        ASSERT_EQ(2u, back[k].values.size());
        EXPECT_EQ("10", back[k].values[0].first);
        EXPECT_EQ(60.0 + k, back[k].values[0].second);
        EXPECT_EQ("11", back[k].values[1].first);
        EXPECT_EQ(2000.0 + k * 50, back[k].values[1].second);
    }
}

TEST(TelemetryPack, roundtrip_through_senml_cbor_wire) {
    // The real path: build -> encode_cbor -> decode_cbor -> parse. Timestamps
    // and values must survive the wire encoding.
    auto recs = t::build_pack("/33000/0/", make_batch());
    std::string wire = sm::encode_cbor(recs);
    ASSERT_FALSE(wire.empty());
    std::vector<sm::Record> decoded;
    ASSERT_EQ(0, sm::decode_cbor(wire, decoded));
    auto back = t::parse_pack(decoded);
    ASSERT_EQ(3u, back.size());
    EXPECT_EQ(1718000000.0,        back[0].timeUnix);
    EXPECT_EQ(1718000000.0 + 10.0, back[2].timeUnix);
    EXPECT_EQ(60.0, back[0].values[0].second);
    EXPECT_EQ(2100.0, back[2].values[1].second);
}

TEST(TelemetryPack, empty_batch_is_empty_pack) {
    EXPECT_TRUE(t::build_pack("/33000/0/", {}).empty());
    EXPECT_TRUE(t::parse_pack({}).empty());
}

TEST(TelemetryPack, float_values_keep_fractional_part) {
    std::vector<t::Sample> b;
    t::Sample s; s.timeUnix = 1718000000.0;
    s.values = { {"15", 5.2} };          // e.g. MAF g/s
    b.push_back(s);
    auto recs = t::build_pack("/33000/0/", b);
    ASSERT_EQ(1u, recs.size());
    EXPECT_TRUE(recs[0].isFloat);
    auto back = t::parse_pack(recs);
    ASSERT_EQ(1u, back.size());
    EXPECT_DOUBLE_EQ(5.2, back[0].values[0].second);
}
