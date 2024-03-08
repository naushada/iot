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

std::int32_t LwM2MAdapter::parseLwM2MUri(const std::string& uri, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid) {

    if(uri.empty() || (uri.at(0) != '/')) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error uri is empty" << std::endl;
        return(-1);
    }

    std::istringstream iss(uri);
    char delim = '/';
    std::ostringstream value;
    iss.rdbuf()->pubsetbuf(const_cast<char *>(uri.data()), uri.length());

    if(uri.at(0) == delim)
        iss.get();
    
    if(iss.get(*value.rdbuf(), delim).good()) {
        oid = std::stoi(value.str());
        /// Get rid of next '/' character
        iss.get();
    } else {
        oid = std::stoi(value.str());
        return(0);
    }

    if(iss.get(*value.rdbuf(), delim).eof() && !value.str().empty()) {
        oiid = std::stoi(value.str());
    } else if(iss.get(*value.rdbuf(), delim).good()) {
        oiid = std::stoi(value.str());
        /// Get rid of next '/' character
        iss.get();
        iss.get(*value.rdbuf(), delim);
        rid = std::stoi(value.str());
    }
    return(0);
}

std::int32_t LwM2MAdapter::parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<TLV>& tlvs) {

    std::uint32_t oid = 0, oiid = 0, rid = 0;
    if(parseLwM2MUri(uri, oid, oiid, rid)) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Unable to parse URI: " << uri << std::endl;
        return(-1);
    }

    LwM2MObject object;
    object.oid = oid;
    object.oiid = oiid;

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

        
        switch(typeValueOf76Bits) {

            case TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00:
            {
                /// The Value part of this TLV is another TLV resource.
                switch(typeValueOf5thBit) {
                    case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                    {
                        /// One byte length
                        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        object.oid = onebyte;

                        break;
                    }
                    case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                    {
                        std::uint16_t twobytes;
                        /// Two Bytes Length
                        if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        object.oid = ntohs(twobytes);

                        break;
                    }
                    default:
                    {
                        std::cout << basename(__FILE__) << ":" << " Error typeValueOf5thBit: " << std::to_string(typeValueOf5thBit) << std::endl;
                    }
                }
                
                break;
            }
                
            case TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01:
                break;
            case TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10:
                break;

            case TypeBits76_ResourceWithValue_11:
            {
                tlv.type = TypeBits76_ResourceWithValue_11;

                switch(typeValueOf5thBit) {
                    case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                    {
                        /// One byte length
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_identifier), 1/*One Byte*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        break;
                    }
                    case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                    {
                        /// Two Bytes Length
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_identifier), 2/*Two Bytes*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_identifier = ntohs(tlv.m_identifier);

                        break;
                    }
                    default:
                    {
                        std::cout << basename(__FILE__) << ":" << " Error typeValueOf5thBit: " << std::to_string(typeValueOf5thBit) << std::endl;
                    }
                }

                switch(typeValueOf43Bits) {
                    case TypeBits43_NoTypeLengthField_00:
                    {
                        //No Type is encoded in TLV
                        tlv.m_length = 0;
                        std::cout << basename(__FILE__) << ":" << " NoLength filed for RID (Resource ID)" << std::endl;
                        break;
                    }
                    case TypeBits43_8BitsTypeLengthField_01:
                    {
                        ///The length of Type filed is one Byte in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_length), 1/*Byte*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        if(!iss.read(reinterpret_cast<char *>(tlv.m_value.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            tlv.m_value.resize(iss.gcount());
                            tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value.resize(iss.gcount());
                        tlv().push_back(tlv);

                        break;
                    }
                    case TypeBits43_16BitsTypeLengthField_10:
                    {
                        ///The length of Type filed is Two Bytes in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_length), 2/*Two Bytes*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_length = ntohs(tlv.m_length);

                        if(!iss.read(reinterpret_cast<char *>(tlv.m_value.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            tlv.m_value.resize(iss.gcount());
                            tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value.resize(iss.gcount());
                        tlv().push_back(tlv);

                        break;
                    }
                    case TypeBits43_24BitsTypeLengthField_11:
                    {
                        ///The length of Type filed is Three Bytes in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_length), 3/*Three Bytes*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        if(!iss.read(reinterpret_cast<char *>(tlv.m_value.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            tlv.m_value.resize(iss.gcount());
                            tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value.resize(iss.gcount());
                        tlv().push_back(tlv);

                        break;
                    }
                    default:
                    {
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << std::endl;
                    }
                }
                
                break;
            }
            default:

        }

    } while(0);
}

std::int32_t LwM2MAdapter::buildLwM2MPayload(const std::string& oid, const std::string& oiid, const std::string& orid, TLV& tlv) {

}






#endif /*__lwm2m_adapter_cpp__*/