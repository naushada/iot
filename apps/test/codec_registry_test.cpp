#include <gtest/gtest.h>

#include "lwm2m_codec_registry.hpp"

TEST(CodecRegistry, REQ_ENC_008_tlv_registered_by_default) {
    // The lwm2m_codec_registry.cpp DefaultRegistrar runs construct-on-load
    // and adds the TLV codec under content-format 11542.
    auto* codec = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_LwM2MTlv);
    ASSERT_NE(nullptr, codec);
    EXPECT_EQ(::lwm2m::CF_LwM2MTlv, codec->format());
}

TEST(CodecRegistry, REQ_ENC_008_unknown_format_returns_null) {
    EXPECT_EQ(nullptr,
              ::lwm2m::CodecRegistry::instance().find(static_cast<std::uint16_t>(0xBEEF)));
}

TEST(CodecRegistry, REQ_ENC_008_replaces_on_duplicate_register) {
    // Re-register a fresh TLV codec; instance pointer must change.
    auto* before = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_LwM2MTlv);
    auto fresh = std::make_shared<::lwm2m::TlvCodec>();
    ::lwm2m::CodecRegistry::instance().add(fresh);
    auto* after = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_LwM2MTlv);
    EXPECT_NE(before, after);
    EXPECT_EQ(fresh.get(), after);
}

TEST(CodecRegistry, REQ_ENC_008_decode_via_registry) {
    auto* codec = ::lwm2m::CodecRegistry::instance().find(::lwm2m::CF_LwM2MTlv);
    ASSERT_NE(nullptr, codec);

    // C1 09 64 = ResourceWithValue rid=9 value=0x64
    std::string wire;
    wire.push_back(static_cast<char>(0xC1));
    wire.push_back(static_cast<char>(0x09));
    wire.push_back(static_cast<char>(0x64));

    LwM2MObject obj;
    EXPECT_EQ(0, codec->decode(wire, obj));
    ASSERT_EQ(1u, obj.m_value.size());
    EXPECT_EQ(9u, obj.m_value[0].m_rid);
    ASSERT_EQ(1u, obj.m_value[0].m_ridvalue.size());
    EXPECT_EQ(0x64, obj.m_value[0].m_ridvalue[0]);
}
