#ifndef __coap_adapter_test_hpp__
#define __coap_adapter_test_hpp__


#include <gtest/gtest.h>
#include <sstream>
#include <vector>

#include "coap_adapter.hpp"
#include "cbor_adapter.hpp"

class CoAPAdapterTest : public ::testing::Test
{
    public:
        CoAPAdapterTest(const std::string& in);
        ~CoAPAdapterTest() = default;
     
        std::string& getFileName() {
            return(fileName);
        }
        
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

    private:
        //std::unique_ptr<CoAPAdapter> coapAdapter;
        //std::unique_ptr<CBORAdapter> cborAdapter;
        std::string fileName;
};









#endif /*__coap_adapter_test_hpp__*/