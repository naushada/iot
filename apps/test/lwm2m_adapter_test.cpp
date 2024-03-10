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
    std::string request = "080079C800144F70656E204D6F62696C6520416C6C69616E6365C801164C696768747765696774204D324D20436C69656E7465C80209333435303030313233C303312E30860641000141010588070842000ED842011388870841007D42010384C10964C10A0F830B410000C40D5182428FC60E2B30323A3030C11055";
    
    LwM2MAdapter lwm2mAdapter;
    //std::vector<LwM2MObject> objects;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    //lwm2mAdapter.parseLwM2MPayload("/3/0", bindata, objects);
    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << ent.m_rid << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
                  //<< std::endl; 
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }

    auto it = std::find_if(object.m_value.begin(), object.m_value.end(), [&](const auto& ent) -> bool {
        //return(ent.m_ridvalue == "Open Mobile Alliance");
        std::string str(ent.m_ridvalue.begin(), ent.m_ridvalue.end());
        return(str == "Open Mobile Alliance");
    });

    EXPECT_TRUE(it != object.m_value.end());
}

TEST(LwM2MAdapterTestSuite, MultipleObjectInstance)
{
    std::string request("080079C800144F70656E204D6F62696C6520416C6C69616E6365C801164C696768747765696774204D324D20436C69656E740AC80209333435303030313233C303312E30860641000141010588070842000ED84201138887084100 7D42010384C10964C10A0F830B410000C40D5182428FC60E2B30323A3030C11055");
    
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << ent.m_rid << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
                  //<< std::endl; 
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }
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