#include <gtest/gtest.h>

#include "cbor_adapter.hpp"
#include "coap_adapter.hpp"

/* ─────────────────────────── BUG-002 + REQ-PUSH-003 ───────────────── */
//
// `readline.cpp` BUG-002 fix can't be exercised without a TTY; the
// regression tests here pin the underlying CBORAdapter::json2cbor
// contract (the failure path that fed the bug) and the structural
// preconditions of the push-plane CoAP request.

TEST(PushRegression, BUG_002_json2cbor_handles_malformed_without_throw) {
    CBORAdapter cb;
    std::string out;
    // Original code threw nlohmann::json::parse_error here — log.txt:9-11.
    EXPECT_EQ(-1, cb.json2cbor("not json at all", out));
    EXPECT_TRUE(out.empty());
}

TEST(PushRegression, BUG_002_json2cbor_produces_cbor_bytes_for_valid_input) {
    CBORAdapter cb;
    std::string out;
    EXPECT_EQ(0, cb.json2cbor(R"([{"key":"v1"}])", out));
    ASSERT_FALSE(out.empty());
    // First byte of an array(1) of map(1) of text("key") → text("v1")
    // is the CBOR array head 0x81. The original bug shipped the literal
    // ASCII '[' (0x5B) under a CBOR content-format; this asserts the
    // output is real CBOR, not text.
    EXPECT_EQ(static_cast<unsigned char>(0x81),
              static_cast<unsigned char>(out[0]));
}

TEST(PushRegression, REQ_PUSH_002_custom_content_formats_present_in_table) {
    // The push plane's custom content-format codes (REQ-PUSH-002) are
    // still resolvable by name through the CoAP adapter's table.
    CoAPAdapter ca;
    EXPECT_EQ("application/timeseries", ca.getContentFormat(12119));
    EXPECT_EQ("application/ucbor",      ca.getContentFormat(12200));
    EXPECT_EQ("application/ucborz",     ca.getContentFormat(12201));
    EXPECT_EQ("application/sucbor",     ca.getContentFormat(12202));
    EXPECT_EQ("application/sucborz",    ca.getContentFormat(12203));
}
