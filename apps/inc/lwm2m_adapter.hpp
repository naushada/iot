#ifndef __lwm2m_adapter_hpp__
#define __lwm2m_adapter_hpp__

#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>


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
struct TLV {
    TypeFieldOfTLV_t m_type;
    std::uint32_t m_identifier;
    std::uint32_t m_length;
    std::vector<std::uint8_t> m_value;
    TLV() : m_type(TypeBits76_ResourceWithValue_11), m_identifier(0),m_length(0), m_value(1024) {}
    ~TLV() = default;
};

struct LwM2MObject {
    /// @brief Object ID
    std::uint32_t oid;
    /// @brief Object Instance ID
    std::uint32_t oiid;
    /// @brief list of Resource IDs
    std::vector<TLV> tlvs;
};
class LwM2MAdapter {
    public:
        LwM2MAdapter();
        ~LwM2MAdapter();

        std::vector<TLV>& tlvs() {
            return(m_tlvs);
        }
        
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
        std::int32_t parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<TLV>& tlvs);
        /**
         * @brief 
         * 
         * @param oid 
         * @param oiid 
         * @param orid 
         * @param tlv 
         * @return std::int32_t 
         */
        std::int32_t buildLwM2MPayload(const std::string& oid, const std::string& oiid, const std::string& orid, TLV& tlv);
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

    private:
        std::vector<LwM2MObject> m_objects;
};

#endif /*__lwm2m_adapter_hpp__*/