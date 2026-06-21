#include <gtest/gtest.h>

#include <sstream>

#include "lwm2m_send_server.hpp"
#include "lwm2m_send.hpp"
#include "lwm2m_telemetry_pack.hpp"
#include "lwm2m_codec_senml.hpp"
#include "lwm2m_coap_builder.hpp"
#include "coap_adapter.hpp"

namespace sv  = ::lwm2m;
namespace snd = ::lwm2m::send;
namespace tp  = ::lwm2m::telemetry;
namespace sm  = ::lwm2m::senml;

namespace {
// Build a real /dp Send frame for a 3-sample batch and parse it to a message.
CoAPAdapter::CoAPMessage make_send_msg(CoAPAdapter& coap, int cf) {
    std::vector<tp::Sample> batch;
    for (int k = 0; k < 3; ++k) {
        tp::Sample s;
        s.timeUnix = 1718000000.0 + k * 5.0;
        s.values = { {"10", 60.0 + k}, {"11", 2000.0 + k * 100} };
        batch.push_back(s);
    }
    auto recs = tp::build_pack("/33000/0/", batch);
    std::string body = (cf == snd::CF_SENML_JSON) ? sm::encode_json(recs)
                                                  : sm::encode_cbor(recs);
    auto wire = snd::build_send_request(0x1111, std::string{0x7F}, body, cf);
    CoAPAdapter::CoAPMessage m;
    coap.parseRequest(wire, m);
    return m;
}

std::uint8_t resp_code(CoAPAdapter& coap, const std::string& bytes) {
    CoAPAdapter::CoAPMessage r;
    coap.parseRequest(bytes, r);
    return r.coapheader.code;
}
} // namespace

TEST(SendServer, decodes_cbor_pack_to_samples_and_acks_204) {
    CoAPAdapter coap;
    auto msg = make_send_msg(coap, snd::CF_SENML_CBOR);

    sv::SendServer srv;
    auto out = srv.handle(msg, coap);

    EXPECT_EQ(sv::SendOutcome::Reported, out.kind);
    EXPECT_EQ("/33000/0/", out.basePath);
    ASSERT_EQ(3u, out.samples.size());
    EXPECT_EQ(1718000000.0,        out.samples[0].timeUnix);
    EXPECT_EQ(1718000000.0 + 10.0, out.samples[2].timeUnix);
    EXPECT_EQ(60.0,   out.samples[0].values[0].second);
    EXPECT_EQ(2200.0, out.samples[2].values[1].second);
    EXPECT_EQ(0x44, resp_code(coap, out.response));    // 2.04 Changed
}

TEST(SendServer, decodes_json_pack_too) {
    CoAPAdapter coap;
    auto msg = make_send_msg(coap, snd::CF_SENML_JSON);
    sv::SendServer srv;
    auto out = srv.handle(msg, coap);
    EXPECT_EQ(sv::SendOutcome::Reported, out.kind);
    ASSERT_EQ(3u, out.samples.size());
    EXPECT_EQ(0x44, resp_code(coap, out.response));
}

TEST(SendServer, non_dp_post_is_None) {
    // A POST to /rd must not be claimed by the Send handler.
    CoAPAdapter coap;
    using namespace ::lwm2m::coap;
    std::ostringstream ss;
    emit_header(ss, 0x2222, std::string{0x01}, METHOD_POST, TYPE_CON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "rd", prev);
    CoAPAdapter::CoAPMessage m;
    coap.parseRequest(ss.str(), m);

    sv::SendServer srv;
    EXPECT_EQ(sv::SendOutcome::None, srv.handle(m, coap).kind);
}

TEST(SendServer, unsupported_content_format_acks_415) {
    // POST /dp but Content-Format 0 (text/plain) → 4.15.
    CoAPAdapter coap;
    using namespace ::lwm2m::coap;
    std::ostringstream ss;
    emit_header(ss, 0x3333, std::string{0x01}, METHOD_POST, TYPE_CON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "dp", prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(0), prev);
    ss.put(static_cast<char>(0xFF)); ss << "plain";
    CoAPAdapter::CoAPMessage m;
    coap.parseRequest(ss.str(), m);

    sv::SendServer srv;
    auto out = srv.handle(m, coap);
    EXPECT_EQ(sv::SendOutcome::UnsupportedFormat, out.kind);
    EXPECT_EQ(0x8F, resp_code(coap, out.response));    // 4.15 Unsupported CF
}

TEST(SendServer, malformed_senml_acks_400) {
    CoAPAdapter coap;
    using namespace ::lwm2m::coap;
    std::ostringstream ss;
    emit_header(ss, 0x4444, std::string{0x01}, METHOD_POST, TYPE_CON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, "dp", prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(snd::CF_SENML_JSON), prev);
    ss.put(static_cast<char>(0xFF)); ss << "{not valid senml";
    CoAPAdapter::CoAPMessage m;
    coap.parseRequest(ss.str(), m);

    sv::SendServer srv;
    auto out = srv.handle(m, coap);
    EXPECT_EQ(sv::SendOutcome::BadRequest, out.kind);
    EXPECT_EQ(0x80, resp_code(coap, out.response));    // 4.00 Bad Request
}
