#ifndef __lwm2m_adapter_test_hpp__
#define __lwm2m_adapter_test_hpp__


#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>

#include "lwm2m_adapter.hpp"

class LwM2MAdapterTest : public ::testing::Test
{
    public:
        LwM2MAdapterTest(const std::string& in);
        ~LwM2MAdapterTest() = default;
     
        std::string& getFileName() {
            return(fileName);
        }
        
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

    private:
        std::string fileName;
};


#endif /*__lwm2m_adapter_test_hpp__*/