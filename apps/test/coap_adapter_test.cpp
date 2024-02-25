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

TEST(CoAPAdapterTestSuite, CoAPZippedPOSTRequest)
{
    std::vector<std::string> cbor;
    std::vector<std::string> out;
    CoAPAdapter coapAdapter;
    bool isBlock = false;
    std::uint8_t method;  
    auto fileContents = coapAdapter.getCBORAdapter().getJson("20240219085111_template_XR90.json");

    if(coapAdapter.buildRequest(fileContents, cbor)) {
        if(coapAdapter.serialise({{"set"}, {"abc123"}, {"xyz"}}, 
                                     {{"ep=6P1532507802A139"}, {"xxp=6P1532507802A139"}}, 
                                     cbor, 12201, 2, out)) {
            for(const auto& ent: out) {
                CoAPAdapter::CoAPMessage coapmessage;
                coapAdapter.parseRequest(ent, coapmessage);
                coapAdapter.dumpCoAPMessage(coapmessage);
                method = coapmessage.coapheader.code;
                if(coapmessage.ismorebitset) {
                    isBlock = true;
                }
            }
        }
    }

    EXPECT_TRUE(isBlock == true && method == 2);
}

TEST(CoAPAdapterTestSuite, CoAPZippedPUTRequest)
{
    std::vector<std::string> cbor;
    std::vector<std::string> out;
    CoAPAdapter coapAdapter;
    bool isBlock = false; 
    std::uint8_t method; 
    auto fileContents = coapAdapter.getCBORAdapter().getJson("20240219085111_template_XR90.json");

    if(coapAdapter.buildRequest(fileContents, cbor)) {
        if(coapAdapter.serialise({{"set"}, {"abc123"}, {"xyz"}}, 
                                     {{"ep=6P1532507802A139"}, {"xxp=6P1532507802A139"}}, 
                                     cbor, 12201, 3/*PUT*/, out)) {
            for(const auto& ent: out) {
                CoAPAdapter::CoAPMessage coapmessage;
                coapAdapter.parseRequest(ent, coapmessage);
                coapAdapter.dumpCoAPMessage(coapmessage);
                method = coapmessage.coapheader.code;
                if(coapmessage.ismorebitset) {
                    isBlock = true;
                }
            }
        }
    }

    EXPECT_TRUE(isBlock == true && method == 3);

}

TEST(CoAPAdapterTestSuite, CoAPZippedDELETERequest)
{
    std::vector<std::string> cbor;
    std::vector<std::string> out;
    CoAPAdapter coapAdapter;
    bool isBlock = false;
    std::uint8_t method;
    auto fileContents = coapAdapter.getCBORAdapter().getJson("20240219085111_template_XR90.json");

    if(coapAdapter.buildRequest(fileContents, cbor)) {
        if(coapAdapter.serialise({{"set"}, {"abc123"}, {"xyz"}}, 
                                     {{"ep=6P1532507802A139"}, {"xxp=6P1532507802A139"}}, 
                                     cbor, 12201, 4/*DELETE*/, out)) {
            for(const auto& ent: out) {
                CoAPAdapter::CoAPMessage coapmessage;
                coapAdapter.parseRequest(ent, coapmessage);
                coapAdapter.dumpCoAPMessage(coapmessage);
                method = coapmessage.coapheader.code;
                if(coapmessage.ismorebitset) {
                    isBlock = true;
                }
            }
        }
    }
    
    EXPECT_TRUE(isBlock == true && method == 4);
}

TEST(CoAPAdapterTestSuite, CoAPSerialisation)
{
    
}

CoAPAdapterTest::CoAPAdapterTest(const std::string& jsonFileName) {
    fileName = jsonFileName;
}




#endif /*__coap_adapter_test_cpp__*/