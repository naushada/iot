#ifndef __lwm2m_adapter_hpp__
#define __lwm2m_adapter_hpp__

#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

extern "C" {
    #include <arpa/inet.h>
    #include <libgen.h>
}

enum ObjectId_t : std::uint32_t {
    SecurityObjectID = 0,
    ServerObjectID = 1,
    AccessControlObjectID = 2,
    DeviceObjectID = 3,
    ConnectivityMonitoringObjectID = 4,
    FirmwareUpdateObjectID = 5,
    LocationObjectID = 6,
    ConnectivityStatisticsObjectID = 7,

};

enum TypeFieldOfTLV_t : std::uint8_t {
  TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00 = 0,
  TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01 = 1,
  TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10 = 2,
  TypeBits76_ResourceWithValue_11 = 3
};

enum LengthOfTheIdentifier_t : std::uint8_t {
    TypeBit5_LengthOfTheIdentifier8BitsLong_0 = 0,
    TypeBit5_LengthOfTheIdentifier16BitsLong_1 = 1,
};

enum LengthOfTheType_t : std::uint8_t {
    TypeBits43_NoTypeLengthField_00 = 0,
    TypeBits43_8BitsTypeLengthField_01 = 1,
    TypeBits43_16BitsTypeLengthField_10 = 2,
    TypeBits43_24BitsTypeLengthField_11 = 3,
};

struct LwM2MObjectData {
    std::uint32_t m_oiid;
    std::uint32_t m_rid;
    std::uint32_t m_riid;
    std::uint32_t m_ridlength;
    std::vector<std::uint8_t> m_ridvalue;

    LwM2MObjectData() : m_oiid(0), m_rid(0), m_riid(0), m_ridlength(0), m_ridvalue(0) {}
    ~LwM2MObjectData() = default;

    LwM2MObjectData& clear() {
        m_oiid = 0;
        m_riid = 0;
        //m_rid = 0;
        m_ridlength = 0;
        m_ridvalue.clear();
        return(*this);
    }
};

struct LwM2MObject {
    /// @brief Object ID
    std::uint32_t m_oid;
    std::vector<LwM2MObjectData>m_value;
    LwM2MObject() : m_oid(0), m_value(0) {}
    ~LwM2MObject() = default;

    LwM2MObject& clear() {
        m_oid = 0;
        m_value.clear();
        return(*this);
    }
};

class LwM2MAdapter {
    public:
        LwM2MAdapter();
        ~LwM2MAdapter();
        
        std::vector<LwM2MObject>& objects() {
            return(m_objects);
        }

        /**
         * @brief 
         * 
         * @param uri 
         * @param payload 
         * @param tlvs 
         * @return std::int32_t 
         */
        std::int32_t parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<LwM2MObject>& objects);
        /**
         * @brief 
         * 
         * @param oid 
         * @param oiid 
         * @param orid 
         * @param tlv 
         * @return std::int32_t 
         */
        std::int32_t buildLwM2MPayload(const ObjectId_t& oid, std::string oiid, json& rids, std::string& out);
        /**
         * @brief 
         * 
         * @param uri 
         * @param oid 
         * @param oiid 
         * @param rid 
         * @return std::int32_t 
         */
        std::int32_t parseLwM2MUri(const std::string& uri, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid);
        /**
         * @brief 
         * 
         * @param payload 
         * @param objectData 
         * @param object 
         * @return std::int32_t 
         */
        std::int32_t parseLwM2MObjects(const std::string& payload, LwM2MObjectData&objectData, LwM2MObject& object);
        /**
         * @brief 
         * 
         * @param oid 
         * @param rid 
         * @return std::string 
         */
        std::string resourceIDName(const std::uint32_t& oid, const std::uint32_t& rid);
        /**
         * @brief 
         * 
         * @param bits76 
         * @param value 
         * @param riid 
         * @param out 
         * @return std::int32_t 
         */
        std::int32_t serialiseTLV(const TypeFieldOfTLV_t& bits76, std::string value, std::uint16_t riid, std::string& out);
        /**
         * @brief 
         * 
         * @param oid 
         * @param value 
         * @param riid 
         * @param out 
         * @return std::int32_t 
         */
        std::int32_t serialiseTLV(const TypeFieldOfTLV_t& oid, std::uint32_t value, std::uint16_t riid, std::string& out);
        /**
         * @brief This function serialises the boolean resource id into LwM2M object
         * 
         * @param oid LwM2M object id which is part of URI
         * @param value boolean value true or false
         * @param riid resource instance id for multiple resources, 0 resource instance is is default
         * @param out Byte stream of LwM2M object
         * @return std::int32_t Upon success 0 else -1
         */
        std::int32_t serialiseTLV(const TypeFieldOfTLV_t& oid, bool value, std::uint16_t riid, std::string& out);
        /**
         * @brief This function encodes/serialise into byte buffer from json object into LwM2M Objects
         * 
         * @param rids json object (json array) or elementary data type (string,boolean or number)
         * @param out byte stream of LwM2M object
         * @return std::int32_t upon success 0 else -1
         */
        std::int32_t serialiseObjects(const json& rids, std::string& out);
        std::int32_t bootstrapSecurityObject00(std::string& out);
        std::int32_t devicemgmtSecurityObject01(std::string& out);
        std::int32_t serverObject30(std::string& out);
        std::int32_t securityObject(std::string& out);

        std::string hexToBinary(const std::string &hex) {
            std::string binary;
            for (size_t i = 0; i < hex.size()/2; ++i) {
                std::stringstream hexToDecimal;
                hexToDecimal << std::hex << hex.substr(2*i, 2);
                int decimal = 0;
                hexToDecimal >> decimal;
                binary.append(1, static_cast<unsigned char>(decimal));
            }
            return binary;
        }

    private:
        std::vector<LwM2MObject> m_objects;
        std::unordered_map<std::uint32_t, std::string> SecurityObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2SecurityObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> ServerObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2ServerObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> AccessControlObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2AccessControlObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> DeviceObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2DeviceObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> ConnectivityMonitoringObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2ConnectivityMonitoringObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> FirmwareUpdateObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2FirmwareUpdateObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> LocationObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2LocationObjectResourceId;
        std::unordered_map<std::uint32_t, std::string> ConnectivityStatsObjectResourceId2ResourceName;
        std::unordered_map<std::string, std::uint32_t> ResourceName2ConnectivityStatsObjectResourceId;
};

#endif /*__lwm2m_adapter_hpp__*/