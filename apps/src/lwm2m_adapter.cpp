#ifndef __lwm2m_adapter_cpp__
#define __lwm2m_adapter_cpp__

extern "C" {
     #include <libgen.h>
}

#include "lwm2m_adapter.hpp"

LwM2MAdapter::LwM2MAdapter() {

}

LwM2MAdapter::~LwM2MAdapter() {

}

std::int32_t LwM2MAdapter::parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<TLV>& tlvs) {
    std::istringstream iss(payload);
    iss.rdbuf()->pubsetbuf(const_cast<char *>(payload.data()), payload.length());
    std::uint8_t onebyte;
    do {
        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
            break;    
        }

        std::uint8_t typeValueOf76Bits = (onebyte & 0b11000000) >> 6;
        std::uint8_t typeValueOf5thBit = (onebyte & 0b00100000) >> 5;
        std::uint8_t typeValueOf43Bits = (onebyte & 0b00011000) >> 3;
        std::uint8_t typeValueOf20Bits = (onebyte & 0b00000111) >> 0;


    } while(0);
}

std::int32_t LwM2MAdapter::buildLwM2MPayload(const std::string& oid, const std::string& oiid, const std::string& orid, TLV& tlv) {

}






#endif /*__lwm2m_adapter_cpp__*/