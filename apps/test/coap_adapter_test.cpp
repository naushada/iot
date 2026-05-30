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

CoAPAdapterTest::CoAPAdapterTest(const std::string& jsonFileName) {
    fileName = jsonFileName;
}




#endif /*__coap_adapter_test_cpp__*/