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

enum LengthFieldOfIdentifier_t : std::uint8_t {
    TypeBit5_LengthOfTheIdentifier8BitsLong_0 = 0,
    TypeBit5_LengthOfTheIdentifier16BitsLong_1 = 1,
};

struct TLV {
    std::uint8_t m_type;
    std::vector<std::uint8_t> m_identifier;
    std::vector<std::uint8_t> m_length;
    std::vector<std::uint8_t> m_value;
    TLV() : m_type(0), m_identifier(2),m_length(3), m_value(1024) {}
    ~TLV() = default;
};

class LwM2MAdapter {
    public:
        LwM2MAdapter();
        ~LwM2MAdapter();

        std::vector<TLV>& tlvs() {
            return(m_tlvs);
        }

        std::int32_t parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<TLV>& tlvs);
        std::int32_t buildLwM2MPayload(const std::string& oid, const std::string& oiid, const std::string& orid, TLV& tlv);

    private:
        std::vector<TLV> m_tlvs;
};

#endif /*__lwm2m_adapter_hpp__*/