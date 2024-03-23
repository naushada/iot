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

TEST(LwM2MAdapterTestSuite, LwM2MUriWithOidOiid) {
    
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    std::uint32_t oid = 0, oiid = 0,rid = 0;
    lwm2mAdapter.parseLwM2MUri("/3/0", oid, oiid, rid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " rid:" << std::to_string(rid) << std::endl;

    EXPECT_TRUE(oid == 3 && oiid == 0);
}

TEST(LwM2MAdapterTestSuite, LwM2MUriWithOid) {
    
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    std::uint32_t oid = 0, oiid = 0,rid = 0;
    lwm2mAdapter.parseLwM2MUri("/3", oid, oiid, rid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " rid:" << std::to_string(rid) << std::endl;

    EXPECT_TRUE(oid == 3);
}


TEST(LwM2MAdapterTestSuite, LwM2MUriWithOidOiidRidSecurityUri) {
    
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    std::uint32_t oid = 0, oiid = 0,rid = 0;
    lwm2mAdapter.parseLwM2MUri("/0/1/11", oid, oiid, rid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " rid:" << std::to_string(rid) << std::endl;

    EXPECT_TRUE(oid == 0 && oiid == 1 && rid == 11);
}

TEST(LwM2MAdapterTestSuite, LwM2MUriWithOidOiidRidDeviceUri) {
    
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    std::uint32_t oid = 0, oiid = 0,rid = 0;
    lwm2mAdapter.parseLwM2MUri("/3/1/10", oid, oiid, rid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " rid:" << std::to_string(rid) << std::endl;

    EXPECT_TRUE(oid == 3 && oiid == 1 && rid == 10);
}

TEST(LwM2MAdapterTestSuite, SingleObjectInstance) {
    std::string request = "080079C800144F70656E204D6F62696C6520416C6C69616E6365C801164C696768747765696774204D324D20436C69656E7465C80209333435303030313233C303312E30860641000141010588070842000ED842011388870841007D42010384C10964C10A0F830B410000C40D5182428FC60E2B30323A3030C11055";
    
    LwM2MAdapter lwm2mAdapter;
    //std::vector<LwM2MObject> objects;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/3/0", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " rid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;

    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", (std::uint8_t)elm);
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
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/3", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " riid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;

    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
                  //<< std::endl; 
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }
}

TEST(LwM2MAdapterTestSuite, BootstrapBSServerSecurityObject) {
    std::string request = "c80019636f6170733a2f2f62732e61697276616e746167652e6e6574c10101c10200c803203031646435666264623038633061373135343632373130373664373865373062c004c80510991832119482a3b5c7d730ce328cb47ac10603c007c008c009c10a00c10b01";
    
    LwM2MAdapter lwm2mAdapter;
    //std::vector<LwM2MObject> objects;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/0", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " riid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;

    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " object.m_oid:" << object.m_oid << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
                  //<< std::endl; 
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }

    //EXPECT_TRUE(it != object.m_value.end());
}

TEST(LwM2MAdapterTestSuite, BootstrapSecurityDMServerObject) {
    std::string request = "c80021636f6170733a2f2f6c772e6e612e61697276616e746167652e6e65743a35363836c10100c10200c803204233373946453136353830344242453843313938334537453431424430433845c804204233373946453136353830344242453843313938334537453431424430433845c8051062f3d1394bc5bc4587db3512e85740d6c10603c007c008c009c10a01c10b01";
    
    LwM2MAdapter lwm2mAdapter;
    //std::vector<LwM2MObject> objects;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/0", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " riid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;
    
    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " object.m_oid:" << object.m_oid << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
                  //<< std::endl; 
        
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }

    //EXPECT_TRUE(it != object.m_value.end());
}

TEST(LwM2MAdapterTestSuite, DMServerObject) {
    std::string request = "c10001c2010384c10201c10601c2075551";
    
    LwM2MAdapter lwm2mAdapter;
    //std::vector<LwM2MObject> objects;
    LwM2MObject object;
    auto bindata = lwm2mAdapter.hexToBinary(request);
    LwM2MObjectData data;
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/1", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " riid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;

    lwm2mAdapter.parseLwM2MObjects(bindata, data, object);
    for(const auto& ent: object.m_value) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " object.m_oid:" << object.m_oid << " ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                  << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                  << " ent.m_ridvalue:";
            
        for(const auto& elm: ent.m_ridvalue) {
            printf("%0.2X ", elm);
        }
        printf("\n");
    }

    //EXPECT_TRUE(it != object.m_value.end());
}

TEST(LwM2MAdapterTestSuite, DeserialiseLwM2MObject) {
    json request = json::array();
    request = {
        {
            {"rid", 0},
            {"value", "Open Mobile Alliance"}
        },
        {
            {"rid", 1},
            {"value", "Lightweight M2M Client"}
        }
    };
    
    std::cout << basename(__FILE__) << ":" << __LINE__ << " request: " << request.dump() << std::endl;
    LwM2MAdapter lwm2mAdapter;
    LwM2MObject object;
    LwM2MObjectData data;
    std::uint32_t oid = 0, oiid = 0,riid = 0;
    lwm2mAdapter.parseLwM2MUri("/0/1", oid, oiid, riid);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " oid:" << std::to_string(oid) << " oiid:" << std::to_string(oiid) << " riid:" << std::to_string(riid) << std::endl;
    object.m_oid = oid;
    data.m_oiid = oiid;
    data.m_riid = riid;
    std::string out = "";
    lwm2mAdapter.buildLwM2MPayload(SecurityObjectID, std::to_string(oiid), request, out);
    for(const auto& elm: out) {
        printf("%0.2X ", static_cast<unsigned char>(elm));
    }
    printf("\n");
    

    //EXPECT_TRUE(it != object.m_value.end());
}

#endif /*__lwm2m_adapter_test_cpp__*/