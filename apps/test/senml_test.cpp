#include <gtest/gtest.h>

#include "lwm2m_codec_registry.hpp"
#include "lwm2m_codec_senml.hpp"

namespace s = ::lwm2m::senml;

/* ─────────────────────────── REQ-ENC-003 SenML JSON ───────────────── */

TEST(SenmlJson, REQ_ENC_003_roundtrip_numeric_int) {
    std::vector<s::Record> in;
    s::Record a;  a.baseName = "/3/0/"; a.name = "9"; a.kind = s::ValueKind::Numeric;
    a.numericValue = 42; a.isFloat = false;
    in.push_back(a);

    auto wire = s::encode_json(in);
    EXPECT_NE(std::string::npos, wire.find("\"bn\":\"/3/0/\""));
    EXPECT_NE(std::string::npos, wire.find("\"v\":42"));

    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ("/3/0/", out[0].baseName);
    EXPECT_EQ("9", out[0].name);
    EXPECT_EQ(s::ValueKind::Numeric, out[0].kind);
    EXPECT_EQ(42.0, out[0].numericValue);
    EXPECT_FALSE(out[0].isFloat);
}

TEST(SenmlJson, REQ_ENC_003_roundtrip_string) {
    std::vector<s::Record> in;
    s::Record a;  a.baseName = "/3/0/"; a.name = "0";
    a.kind = s::ValueKind::String; a.stringValue = "Acme Co";
    in.push_back(a);

    auto wire = s::encode_json(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(s::ValueKind::String, out[0].kind);
    EXPECT_EQ("Acme Co", out[0].stringValue);
}

TEST(SenmlJson, REQ_ENC_003_roundtrip_boolean) {
    std::vector<s::Record> in;
    s::Record a;  a.baseName = "/0/1/"; a.name = "1";
    a.kind = s::ValueKind::Boolean; a.booleanValue = true;
    in.push_back(a);
    auto wire = s::encode_json(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    EXPECT_TRUE(out[0].booleanValue);
}

TEST(SenmlJson, REQ_ENC_003_roundtrip_data) {
    std::vector<s::Record> in;
    s::Record a;  a.baseName = "/0/0/"; a.name = "3";
    a.kind = s::ValueKind::Data; a.dataValue = std::string("\x00\x01\xFE\xFF", 4);
    in.push_back(a);
    auto wire = s::encode_json(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    ASSERT_EQ(s::ValueKind::Data, out[0].kind);
    EXPECT_EQ(4u, out[0].dataValue.size());
    EXPECT_EQ('\x00', out[0].dataValue[0]);
    EXPECT_EQ('\xFF', out[0].dataValue[3]);
}

TEST(SenmlJson, REQ_ENC_003_bn_accumulates_across_records) {
    // RFC 8428 §4.5: subsequent records inherit bn from the first.
    std::string wire = R"([{"bn":"/3/0/","n":"0","vs":"Acme"},
                          {"n":"17","vs":"sensor"},
                          {"n":"9","v":42}])";
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    ASSERT_EQ(3u, out.size());
    EXPECT_EQ("/3/0/", out[0].baseName);
    EXPECT_EQ("/3/0/", out[1].baseName);
    EXPECT_EQ("/3/0/", out[2].baseName);
    EXPECT_EQ("/3/0/17", out[1].path());
}

TEST(SenmlJson, REQ_ENC_003_decode_rejects_multiple_value_fields) {
    std::vector<s::Record> out;
    EXPECT_EQ(-1, s::decode_json(R"([{"n":"0","v":1,"vs":"oops"}])", out));
}

TEST(SenmlJson, REQ_ENC_003_decode_rejects_bn_not_on_first) {
    std::vector<s::Record> out;
    EXPECT_EQ(-1, s::decode_json(R"([{"n":"0"},{"bn":"/x","n":"1"}])", out));
}

TEST(SenmlJson, REQ_ENC_003_decode_rejects_non_array) {
    std::vector<s::Record> out;
    EXPECT_EQ(-1, s::decode_json(R"({"n":"0"})", out));
    EXPECT_EQ(-1, s::decode_json("not json", out));
}

/* ───── SenML time fields (bt/t) — timestamped telemetry batches ───── */

TEST(SenmlJson, time_roundtrip_bt_and_per_record_t) {
    // A 3-sample batch of /33000/0/10 (speed): base time + per-record offsets.
    std::vector<s::Record> in;
    for (int k = 0; k < 3; ++k) {
        s::Record r;
        r.baseName = "/33000/0/"; r.name = "10";
        r.kind = s::ValueKind::Numeric; r.numericValue = 60 + k; r.isFloat = false;
        r.hasBaseTime = true; r.baseTime = 1718000000.0;   // bt on first record only
        r.hasTime = true;     r.time = k * 2.0;             // +0, +2, +4 s
        in.push_back(r);
    }

    auto wire = s::encode_json(in);
    EXPECT_NE(std::string::npos, wire.find("\"bt\":1718000000"));
    EXPECT_NE(std::string::npos, wire.find("\"t\":4"));

    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    ASSERT_EQ(3u, out.size());
    for (int k = 0; k < 3; ++k) {
        EXPECT_TRUE(out[k].hasBaseTime);
        EXPECT_EQ(1718000000.0, out[k].baseTime);    // bt accumulates to all
        EXPECT_TRUE(out[k].hasTime);
        EXPECT_EQ(k * 2.0, out[k].time);
        EXPECT_EQ(1718000000.0 + k * 2.0, out[k].effectiveTime());
    }
}

TEST(SenmlJson, time_decode_rejects_bt_not_on_first) {
    std::vector<s::Record> out;
    EXPECT_EQ(-1, s::decode_json(R"([{"n":"0","v":1},{"bt":1,"n":"1","v":2}])", out));
}

TEST(SenmlJson, time_absent_means_no_time_flags) {
    std::vector<s::Record> in;
    s::Record a; a.baseName = "/3/0/"; a.name = "9";
    a.kind = s::ValueKind::Numeric; a.numericValue = 42;
    in.push_back(a);
    auto wire = s::encode_json(in);
    EXPECT_EQ(std::string::npos, wire.find("\"bt\""));   // no time emitted
    EXPECT_EQ(std::string::npos, wire.find("\"t\""));
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_json(wire, out));
    EXPECT_FALSE(out[0].hasBaseTime);
    EXPECT_FALSE(out[0].hasTime);
}

/* ─────────────────────────── REQ-ENC-004 SenML CBOR ───────────────── */

TEST(SenmlCbor, REQ_ENC_004_roundtrip_numeric_int) {
    std::vector<s::Record> in;
    s::Record a;  a.baseName = "/3/0/"; a.name = "9";
    a.kind = s::ValueKind::Numeric; a.numericValue = 42;
    in.push_back(a);

    auto wire = s::encode_cbor(in);
    // First byte = major-type=4 (array) | length=1 = 0x81
    ASSERT_FALSE(wire.empty());
    EXPECT_EQ(static_cast<char>(0x81), wire[0]);

    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_cbor(wire, out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ("/3/0/", out[0].baseName);
    EXPECT_EQ("9", out[0].name);
    EXPECT_EQ(42.0, out[0].numericValue);
    EXPECT_FALSE(out[0].isFloat);
}

TEST(SenmlCbor, REQ_ENC_004_roundtrip_float) {
    std::vector<s::Record> in;
    s::Record a; a.baseName = "/3/0/"; a.name = "10";
    a.kind = s::ValueKind::Numeric; a.numericValue = 1.5; a.isFloat = true;
    in.push_back(a);
    auto wire = s::encode_cbor(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_cbor(wire, out));
    EXPECT_TRUE(out[0].isFloat);
    EXPECT_EQ(1.5, out[0].numericValue);
}

TEST(SenmlCbor, time_roundtrip_bt_and_t) {
    // Same timestamped batch as the JSON test, over CBOR. Integral times are
    // emitted as compact CBOR uints (emit_time) and read back via read_number.
    std::vector<s::Record> in;
    for (int k = 0; k < 3; ++k) {
        s::Record r;
        r.baseName = "/33000/0/"; r.name = "10";
        r.kind = s::ValueKind::Numeric; r.numericValue = 60 + k;
        r.hasBaseTime = true; r.baseTime = 1718000000.0;
        r.hasTime = true;     r.time = k * 2.0;
        in.push_back(r);
    }
    auto wire = s::encode_cbor(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_cbor(wire, out));
    ASSERT_EQ(3u, out.size());
    for (int k = 0; k < 3; ++k) {
        EXPECT_TRUE(out[k].hasBaseTime);
        EXPECT_EQ(1718000000.0, out[k].baseTime);
        EXPECT_EQ(k * 2.0, out[k].time);
        EXPECT_EQ(1718000000.0 + k * 2.0, out[k].effectiveTime());
    }
}

TEST(SenmlCbor, REQ_ENC_004_roundtrip_string_bool_data) {
    std::vector<s::Record> in;
    {
        s::Record a; a.baseName = "/3/0/"; a.name = "0";
        a.kind = s::ValueKind::String; a.stringValue = "Acme";
        in.push_back(a);
    }
    {
        s::Record a; a.baseName = "/3/0/"; a.name = "1";
        a.kind = s::ValueKind::Boolean; a.booleanValue = true;
        in.push_back(a);
    }
    {
        s::Record a; a.baseName = "/3/0/"; a.name = "3";
        a.kind = s::ValueKind::Data; a.dataValue = std::string("\xDE\xAD\xBE\xEF", 4);
        in.push_back(a);
    }

    auto wire = s::encode_cbor(in);
    std::vector<s::Record> out;
    ASSERT_EQ(0, s::decode_cbor(wire, out));
    ASSERT_EQ(3u, out.size());
    EXPECT_EQ("Acme", out[0].stringValue);
    EXPECT_TRUE(out[1].booleanValue);
    EXPECT_EQ(4u, out[2].dataValue.size());
    EXPECT_EQ('\xEF', out[2].dataValue[3]);
}

TEST(SenmlCbor, REQ_ENC_004_integer_label_neg2_for_bn) {
    // Spot check: the wire must use the integer label -2 (CBOR negative
    // int 0 → encoded as 0x21) for bn. The first map's first key after
    // the map header should be 0x21.
    std::vector<s::Record> in;
    s::Record a; a.baseName = "/x"; a.name = "y";
    a.kind = s::ValueKind::String; a.stringValue = "z";
    in.push_back(a);
    auto wire = s::encode_cbor(in);
    // [array(1)][map(3)][key=-2 (=0x21)] ...
    ASSERT_GT(wire.size(), 2u);
    EXPECT_EQ(static_cast<char>(0x21), wire[2]);
}

TEST(SenmlCbor, REQ_ENC_004_decode_rejects_truncated) {
    std::vector<s::Record> out;
    std::string truncated;
    truncated.push_back(static_cast<char>(0x81));   // array(1)
    truncated.push_back(static_cast<char>(0xA2));   // map(2) but no entries
    EXPECT_EQ(-1, s::decode_cbor(truncated, out));
}

/* ─────────────────────────── Codec registry ───────────────────────── */

TEST(SenmlRegistry, REQ_ENC_008_both_codecs_registered) {
    auto* j = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_SenmlJson);
    auto* c = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_SenmlCbor);
    ASSERT_NE(nullptr, j);
    ASSERT_NE(nullptr, c);
    EXPECT_EQ(::lwm2m::CF_SenmlJson, j->format());
    EXPECT_EQ(::lwm2m::CF_SenmlCbor, c->format());
}

TEST(SenmlRegistry, decode_via_registry_lossy_but_present) {
    auto* j = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_SenmlJson);
    ASSERT_NE(nullptr, j);
    LwM2MObject obj;
    int rc = j->decode(R"([{"bn":"/3/0/","n":"9","v":42}])", obj);
    EXPECT_EQ(0, rc);
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(9u, obj.m_value[0].m_rid);
    EXPECT_EQ("42", std::string(obj.m_value[0].m_ridvalue.begin(),
                                obj.m_value[0].m_ridvalue.end()));
}
