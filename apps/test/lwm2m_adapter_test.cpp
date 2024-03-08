#ifndef __lwm2m_adapter_test_cpp__
#define __lwm2m_adapter_test_cpp__

#include "lwm2m_adapter_test.hpp"

void LwM2MAdapterTest::SetUp()
{
    
}

void LwM2MAdapterTest::TearDown()
{
    
}

void LwM2MAdapterTest::TestBody()
{

}

TEST(LwM2MAdapterTestSuite, SingleObjectInstance) {
    std::string request = "C800144F70656E204D6F62696C6520416C6C69616E6365C801164C696768747765696774204D324D20436C6965"
                          "6E74C80209333435303030313233C303312E30860641000141010588070842000ED842011388870841007D42010384C10964"
                          "C10A0F830B410000C40D5182428FC60E2B30323A3030C11055";
    
    LwM2MAdapter lwm2mAdapter;
    std::vector<LwM2MObject> objects;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    lwm2mAdapter.parseLwM2MPayload("/3/0", bindata, objects);

    for(const auto& object: objects) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " object.oid:" << object.oid << " object.oiid:" << object.oiid << std::endl;
        for(const auto& ent: object.tlvs) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " ent.m_type:" << ent.m_type << " ent.m_identifier:" << ent.m_identifier << 
            " ent.m_length:" << ent.m_length << std::endl;
        }
    }
    

    //EXPECT_TRUE(isBlock == true && method == 2);
}

TEST(LwM2MAdapterTestSuite, CoAPZippedPUTRequest)
{
    //EXPECT_TRUE(isBlock == true && method == 3);

}

TEST(LwM2MAdapterTestSuite, CoAPZippedDELETERequest)
{
    //EXPECT_TRUE(isBlock == true && method == 4);
}

TEST(LwM2MAdapterTestSuite, CoAPSerialisation)
{
    
}

LwM2MAdapterTest::LwM2MAdapterTest(const std::string& jsonFileName) {
    fileName = jsonFileName;
}


#endif /*__lwm2m_adapter_test_cpp__*/