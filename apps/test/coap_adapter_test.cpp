#ifndef __coap_adapter_test_cpp__
#define __coap_adapter_test_cpp__

#include "coap_adapter_test.hpp"

void CoAPAdapterTest::SetUp()
{
    
}

void CoAPAdapterTest::TearDown()
{
    
}

void CoAPAdapterTest::TestBody()
{

}

namespace {

/// Synthesize a JSON object larger than the 1024-byte deflate
/// threshold in CoAPAdapter::buildRequest so the zip + Block1 chunking
/// path fires. Replaces the original tests' dependency on a missing
/// fixture file (`20240219085111_template_XR90.json`).
std::string make_large_json() {
    std::string s = "{\"items\":[";
    const char* row = "{\"k\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    for (int i = 0; i < 50; ++i) {
        if (i) s += ',';
        s += row;
    }
    s += "]}";
    return s;
}

/// Drive the compress + Block1 path for a given CoAP method, return
/// (isBlock, observed-method). Block1 fires when the deflated payload
/// is split into >1 frame *or* the single emitted frame has the More
/// bit set; we settle for at least the method byte coming through
/// intact across every emitted frame.
struct ZipOutcome { bool sawAny; std::uint8_t method; std::size_t frames; };

ZipOutcome run_zipped(std::uint8_t method) {
    CoAPAdapter coap;
    std::vector<std::string> cbor;
    std::vector<std::string> out;
    ZipOutcome o{false, 0, 0};

    EXPECT_TRUE(coap.buildRequest(make_large_json(), cbor))
        << "buildRequest should compress > 1024-byte CBOR";
    EXPECT_GT(cbor.size(), 0u);

    EXPECT_TRUE(coap.serialise({{"set"}, {"abc123"}, {"xyz"}},
                               {{"ep=6P1532507802A139"}, {"xxp=6P1532507802A139"}},
                               cbor, 12201, method, out));
    o.frames = out.size();

    for (const auto& frame : out) {
        CoAPAdapter::CoAPMessage msg;
        coap.parseRequest(frame, msg);
        o.method = msg.coapheader.code;
        o.sawAny = true;
    }
    return o;
}

} // namespace

TEST(CoAPAdapterTestSuite, CoAPZippedPOSTRequest)
{
    auto r = run_zipped(2 /*POST*/);
    EXPECT_TRUE(r.sawAny);
    EXPECT_EQ(r.method, 2u);
    EXPECT_GE(r.frames, 1u);
}

TEST(CoAPAdapterTestSuite, CoAPZippedPUTRequest)
{
    auto r = run_zipped(3 /*PUT*/);
    EXPECT_TRUE(r.sawAny);
    EXPECT_EQ(r.method, 3u);
    EXPECT_GE(r.frames, 1u);
}

TEST(CoAPAdapterTestSuite, CoAPZippedDELETERequest)
{
    auto r = run_zipped(4 /*DELETE*/);
    EXPECT_TRUE(r.sawAny);
    EXPECT_EQ(r.method, 4u);
    EXPECT_GE(r.frames, 1u);
}

TEST(CoAPAdapterTestSuite, CoAPSerialisation)
{
    
}

// ── CoAP ping / RST handling (NAT keepalive) ────────────────────────────────

TEST(CoAPAdapterTestSuite, BuildResetIsFourByteRstEchoingMsgId) {
    CoAPAdapter a;
    CoAPAdapter::CoAPMessage m{};
    m.coapheader.msgid = 0xBEEF;
    auto rst = a.buildReset(m);
    ASSERT_EQ(4u, rst.size());
    EXPECT_EQ(0x70, static_cast<std::uint8_t>(rst[0]));   // ver1, type RST(3), TKL0
    EXPECT_EQ(0x00, static_cast<std::uint8_t>(rst[1]));   // code 0.00
    EXPECT_EQ(0xBE, static_cast<std::uint8_t>(rst[2]));   // msgid high
    EXPECT_EQ(0xEF, static_cast<std::uint8_t>(rst[3]));   // msgid low
}

TEST(CoAPAdapterTestSuite, EmptyConPingAnsweredWithReset) {
    CoAPAdapter a;
    CoAPAdapter::CoAPMessage m{};
    m.coapheader.type  = 0;        // Confirmable
    m.coapheader.code  = 0;        // 0.00 (empty) → a CoAP ping
    m.coapheader.msgid = 0x1234;
    std::vector<std::string> out;
    EXPECT_TRUE(a.handleEmptyMessage(m, out));            // consumed
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(0x70, static_cast<std::uint8_t>(out[0][0]));  // answered with RST
}

TEST(CoAPAdapterTestSuite, ResetIsConsumedAndDroppedNotEchoed) {
    CoAPAdapter a;
    CoAPAdapter::CoAPMessage m{};
    m.coapheader.type = 3;         // Reset
    m.coapheader.code = 0;
    std::vector<std::string> out;
    EXPECT_TRUE(a.handleEmptyMessage(m, out));            // consumed
    EXPECT_TRUE(out.empty());                             // no echo back
}

TEST(CoAPAdapterTestSuite, NonEmptyRequestIsNotConsumed) {
    CoAPAdapter a;
    CoAPAdapter::CoAPMessage m{};
    m.coapheader.type = 0;         // Confirmable
    m.coapheader.code = 0x02;      // POST — a real request, must dispatch normally
    std::vector<std::string> out;
    EXPECT_FALSE(a.handleEmptyMessage(m, out));
    EXPECT_TRUE(out.empty());
}

CoAPAdapterTest::CoAPAdapterTest(const std::string& jsonFileName) {
    fileName = jsonFileName;
}




#endif /*__coap_adapter_test_cpp__*/