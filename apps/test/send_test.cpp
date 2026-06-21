#include <gtest/gtest.h>

#include "lwm2m_send.hpp"
#include "lwm2m_telemetry_pack.hpp"
#include "lwm2m_codec_senml.hpp"
#include "coap_adapter.hpp"

namespace snd = ::lwm2m::send;
namespace tp  = ::lwm2m::telemetry;
namespace sm  = ::lwm2m::senml;

namespace {
// Pull the Uri-Path segments + Content-Format value out of a parsed frame.
void collect(CoAPAdapter& coap, const CoAPAdapter::CoAPMessage& m,
             std::vector<std::string>& path, int& cf) {
    cf = -1;
    for (const auto& opt : m.uripath) {
        const std::string num = coap.getOptionNumber(opt.optiondelta);
        if (num == "Uri-Path") path.push_back(opt.optionvalue);
        else if (num == "Content-Format" && !opt.optionvalue.empty()) {
            cf = 0;
            for (unsigned char b : opt.optionvalue) cf = (cf << 8) | b;  // CoAP uint
        }
    }
}
} // namespace

TEST(Send, build_request_is_post_dp_senml_cbor) {
    auto wire = snd::build_send_request(0x2A2A, std::string{0x55},
                                        "payloadbytes", snd::CF_SENML_CBOR);
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);

    EXPECT_EQ(0x2A2A, parsed.coapheader.msgid);
    EXPECT_EQ(2u, parsed.coapheader.code);          // POST

    std::vector<std::string> path; int cf;
    collect(coap, parsed, path, cf);
    ASSERT_EQ(1u, path.size());
    EXPECT_EQ("dp", path[0]);
    EXPECT_EQ(112, cf);                              // SenML CBOR
    EXPECT_EQ("payloadbytes", parsed.payload);
}

TEST(Send, empty_payload_has_no_marker) {
    auto wire = snd::build_send_request(1, std::string{0x01}, "", snd::CF_SENML_CBOR);
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    EXPECT_EQ(2u, parsed.coapheader.code);
    EXPECT_TRUE(parsed.payload.empty());
}

TEST(Send, full_pipeline_pack_to_send_frame_and_back) {
    // build_pack -> encode_cbor -> build_send_request -> parseRequest ->
    // decode_cbor(payload) -> parse_pack : timestamps + values survive the
    // entire client Send framing.
    std::vector<tp::Sample> batch;
    for (int k = 0; k < 3; ++k) {
        tp::Sample s;
        s.timeUnix = 1718000000.0 + k * 5.0;
        s.values = { {"10", 60.0 + k}, {"11", 2000.0 + k * 100} };
        batch.push_back(s);
    }
    std::string cbor = sm::encode_cbor(tp::build_pack("/33000/0/", batch));
    ASSERT_FALSE(cbor.empty());

    auto wire = snd::build_send_request(0x0007, std::string{0x42}, cbor);
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(wire, parsed);
    ASSERT_EQ(cbor, parsed.payload);                // payload survived framing

    std::vector<sm::Record> recs;
    ASSERT_EQ(0, sm::decode_cbor(parsed.payload, recs));
    auto back = tp::parse_pack(recs);
    ASSERT_EQ(3u, back.size());
    EXPECT_EQ(1718000000.0,        back[0].timeUnix);
    EXPECT_EQ(1718000000.0 + 10.0, back[2].timeUnix);
    EXPECT_EQ(60.0,   back[0].values[0].second);
    EXPECT_EQ(2200.0, back[2].values[1].second);
}
