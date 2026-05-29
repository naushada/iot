#include <gtest/gtest.h>

#include "lwm2m_codec_tlv.hpp"

namespace {

std::string hex(const std::string& bytes) {
    static const char* k = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(k[c >> 4]);
        out.push_back(k[c & 0xF]);
    }
    return out;
}

std::string from_hex(const std::string& h) {
    std::string out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        out.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
    }
    return out;
}

} // namespace

/* ─────────────────────────── REQ-ENC-001 roundtrip per type ──────────── */

TEST(Tlv, REQ_ENC_001_roundtrip_string) {
    std::string enc;
    ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11,
                                "Open Mobile Alliance", 0, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(0u,  obj.m_value[0].m_rid);
    EXPECT_EQ(20u, obj.m_value[0].m_ridlength);
    EXPECT_EQ("Open Mobile Alliance",
              std::string(obj.m_value[0].m_ridvalue.begin(),
                          obj.m_value[0].m_ridvalue.end()));
}

TEST(Tlv, REQ_ENC_001_roundtrip_uint_1byte) {
    std::string enc;
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceWithValue_11, 0x64, 9, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(9u, obj.m_value[0].m_rid);
    ASSERT_EQ(1u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x64, obj.m_value[0].m_ridvalue[0]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_uint_2byte) {
    std::string enc;
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceWithValue_11, 0x0ED8, 7, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(7u, obj.m_value[0].m_rid);
    ASSERT_EQ(2u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x0E, obj.m_value[0].m_ridvalue[0]);
    EXPECT_EQ(0xD8, obj.m_value[0].m_ridvalue[1]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_bool_true) {
    std::string enc;
    ::lwm2m::tlv::encode_bool(TypeBits76_ResourceWithValue_11, true, 1, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(1u, obj.m_value[0].m_rid);
    ASSERT_EQ(1u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x01, obj.m_value[0].m_ridvalue[0]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_bool_false) {
    std::string enc;
    ::lwm2m::tlv::encode_bool(TypeBits76_ResourceWithValue_11, false, 1, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    ASSERT_EQ(1u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x00, obj.m_value[0].m_ridvalue[0]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_time_positive) {
    // 2024-01-01 00:00:00 UTC = 0x6593B400
    std::string enc;
    ::lwm2m::tlv::encode_time(TypeBits76_ResourceWithValue_11,
                              0x6593B400, 13, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(13u, obj.m_value[0].m_rid);
    ASSERT_EQ(4u,  obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x65, obj.m_value[0].m_ridvalue[0]);
    EXPECT_EQ(0x93, obj.m_value[0].m_ridvalue[1]);
    EXPECT_EQ(0xB4, obj.m_value[0].m_ridvalue[2]);
    EXPECT_EQ(0x00, obj.m_value[0].m_ridvalue[3]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_time_small_fits_one_byte) {
    std::string enc;
    ::lwm2m::tlv::encode_time(TypeBits76_ResourceWithValue_11, 7, 13, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    ASSERT_EQ(1u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x07, obj.m_value[0].m_ridvalue[0]);
}

TEST(Tlv, REQ_ENC_001_roundtrip_objlink) {
    std::string enc;
    ::lwm2m::tlv::encode_objlink(TypeBits76_ResourceWithValue_11,
                                 /*targetOid*/ 1, /*targetIid*/ 0,
                                 /*id*/ 21, enc);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(enc, scratch, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(21u, obj.m_value[0].m_rid);
    ASSERT_EQ(4u,  obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x00, obj.m_value[0].m_ridvalue[0]);
    EXPECT_EQ(0x01, obj.m_value[0].m_ridvalue[1]);
    EXPECT_EQ(0x00, obj.m_value[0].m_ridvalue[2]);
    EXPECT_EQ(0x00, obj.m_value[0].m_ridvalue[3]);
}

/* ─────────────────────────── REQ-ENC-001 multi-resource ──────────────── */

TEST(Tlv, REQ_ENC_001_multi_resource_riid_propagated) {
    // Construct a MultipleResource container holding two Resource Instance
    // records under RID 6 (Available Power Sources). riids 0 and 1.
    std::string inner1, inner2;
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01,
                              1, /*riid*/ 0, inner1);
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01,
                              5, /*riid*/ 1, inner2);

    std::string container;
    ::lwm2m::tlv::encode_string(TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10,
                                inner1 + inner2, /*rid*/ 6, container);

    LwM2MObject obj;
    LwM2MObjectData scratch;
    ASSERT_EQ(0, ::lwm2m::tlv::decode_container(container, scratch, obj));
    ASSERT_EQ(2u, obj.m_value.size());
    // BUG-007 fix: both inner records inherit rid=6 from the outer.
    EXPECT_EQ(6u, obj.m_value[0].m_rid);
    EXPECT_EQ(6u, obj.m_value[1].m_rid);
    EXPECT_EQ(0u, obj.m_value[0].m_riid);
    EXPECT_EQ(1u, obj.m_value[1].m_riid);
}

/* ─────────────────────────── REQ-ENC-002 malformed input ─────────────── */

TEST(Tlv, REQ_ENC_002_truncated_header_rejected) {
    // Empty payload is valid (zero records). A single-byte buffer
    // declaring an 8-bit identifier with no identifier byte is not.
    std::string truncated;
    truncated.push_back(static_cast<char>(0xC0));  // ResourceWithValue, id=8b, len=0
    LwM2MObject obj;
    LwM2MObjectData scratch;
    EXPECT_EQ(-1, ::lwm2m::tlv::decode_container(truncated, scratch, obj));
}

TEST(Tlv, REQ_ENC_002_length_overflow_rejected) {
    // ResourceWithValue, id=8b, length-field=8-bit, length=0xFF; but only
    // 3 bytes of value provided.
    std::string bad = from_hex("C8" "01" "FF" "010203");
    LwM2MObject obj;
    LwM2MObjectData scratch;
    EXPECT_EQ(-1, ::lwm2m::tlv::decode_container(bad, scratch, obj));
}

TEST(Tlv, REQ_ENC_002_empty_payload_ok) {
    LwM2MObject obj;
    LwM2MObjectData scratch;
    EXPECT_EQ(0, ::lwm2m::tlv::decode_container(std::string{}, scratch, obj));
    EXPECT_EQ(0u, obj.m_value.size());
}

/* ─────────────────────────── byte-shape spot-check ───────────────────── */

TEST(Tlv, ByteShape_string_one_byte_identifier) {
    std::string enc;
    ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11, "OMA", 0, enc);
    // Type=11000000 (rwval, id-8b, nolen, inline-len=3) = 0xC3
    // Id = 0x00
    // Value = "OMA"
    EXPECT_EQ("C3004F4D41", hex(enc));
}

TEST(Tlv, ByteShape_uint_value_64_id_9) {
    std::string enc;
    ::lwm2m::tlv::encode_uint(TypeBits76_ResourceWithValue_11, 0x64, 9, enc);
    // Type=11000001 = 0xC1, Id=0x09, Value=0x64
    EXPECT_EQ("C10964", hex(enc));
}
