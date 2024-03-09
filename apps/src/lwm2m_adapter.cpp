#ifndef __lwm2m_adapter_cpp__
#define __lwm2m_adapter_cpp__

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
        //return(0);
    }

    if(iss.get(*value.rdbuf(), delim).eof() && !value.str().empty()) {
        oiid = std::stoi(value.str());
    } else if(iss.get(*value.rdbuf(), delim).eof()) {
        oiid = std::stoi(value.str());
        /// Get rid of next '/' character
        iss.get();
        iss.get(*value.rdbuf(), delim);
        rid = std::stoi(value.str());
    }
    return(0);
}

std::int32_t LwM2MAdapter::parseLwM2MObjects(const std::string& payload, LwM2MObjectData& data, LwM2MObject& object) {
    std::istringstream iss;
    iss.rdbuf()->pubsetbuf(const_cast<char *>(payload.data()), payload.length());
    std::uint8_t onebyte;

    if(iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).eof()) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " End of stream " << std::endl;
        return(0);
    }

    std::uint8_t typeValueOf76Bits = (onebyte & 0b11000000) >> 6;
    std::uint8_t typeValueOf5thBit = (onebyte & 0b00100000) >> 5;
    std::uint8_t typeValueOf43Bits = (onebyte & 0b00011000) >> 3;
    std::uint8_t typeValueOf20Bits = (onebyte & 0b00000111) >> 0;

    std::cout << basename(__FILE__) << ":" << __LINE__ << " typeValueOf76Bits:" << std::to_string(typeValueOf76Bits) << " typeValueOf5thBit:" << std::to_string(typeValueOf5thBit)
              << " typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << " typeValueOf20Bits:" << std::to_string(typeValueOf20Bits) << std::endl;

    if(typeValueOf76Bits == TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00) {

        /// uri has just Object Id no instance Id.
        switch(typeValueOf5thBit) {
            case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                {
                    /// One byte is the object instance id
                    if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_oiid = static_cast<std::uint32_t>(onebyte);

                    break;
                }

            case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                {
                    /// Two Bytes are the object instance id
                    std::uint16_t twobytes;
                    if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_oiid = static_cast<std::uint32_t>(ntohs(twobytes));

                    break;
                }

            default:
                {
                    std::cout << basename(__FILE__) << ":" << " Error Object Instance ID can't be greater than 2 bytes: " << std::to_string(typeValueOf5thBit) << std::endl;
                }
        }

        std::uint32_t len = 0;
        if(typeValueOf43Bits == TypeBits43_NoTypeLengthField_00) {

            len = static_cast<std::uint32_t>(typeValueOf20Bits);

        } else if(typeValueOf43Bits == TypeBits43_8BitsTypeLengthField_01) {

            if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
            }

            len = static_cast<std::uint32_t>(onebyte);

        } else if(typeValueOf43Bits == TypeBits43_16BitsTypeLengthField_10) {

            std::uint16_t twobytes;
            if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                return(-1);    
            }
            len = static_cast<std::uint32_t>(ntohs(twobytes));

        } else {

        }

        /// Read len number of bytes
        std::vector<std::uint8_t> contents(len);
        if(!iss.read(reinterpret_cast<char *>(contents.data()), contents.size()).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
        }
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");

        //std::string newcontents(contents.begin(), contents.end());
        parseLwM2MObjects(std::string(contents.begin(), contents.end()), data, object);

    } else if(typeValueOf76Bits == TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01) {

        switch(typeValueOf5thBit) {

            case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                {
                    /// One byte is the resource id
                    if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_riid = static_cast<std::uint32_t>(onebyte);

                    break;
                }

            case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                {
                    /// Two Bytes are the resource id
                    std::uint16_t twobytes;
                    if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_riid = static_cast<std::uint32_t>(ntohs(twobytes));

                    break;
                }

            default:
                {
                    std::cout << basename(__FILE__) << ":" << " Error Resource ID can't be greater than 2 bytes: " << std::to_string(typeValueOf5thBit) << std::endl;
                }
        }

        std::uint32_t len = 0;
        if(typeValueOf43Bits == TypeBits43_NoTypeLengthField_00) {

            len = static_cast<std::uint32_t>(typeValueOf20Bits);

        } else if(typeValueOf43Bits == TypeBits43_8BitsTypeLengthField_01) {

            if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
            }

            len = static_cast<std::uint32_t>(onebyte);

        } else if(typeValueOf43Bits == TypeBits43_16BitsTypeLengthField_10) {

            std::uint16_t twobytes;
            if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                return(-1);    
            }
            len = static_cast<std::uint32_t>(ntohs(twobytes));

        } else {

        }

        /// Read len number of bytes
        std::vector<std::uint8_t> contents(len);
        if(!iss.read(reinterpret_cast<char *>(contents.data()), contents.size()).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
        }

        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");

        data.m_ridlength = len;
        data.m_ridvalue = contents;
        std::cout << basename(__FILE__) << ":" << __LINE__ << " data.m_riid:" << data.m_riid << " data.m_rid:" << data.m_rid << " data.m_ridlength:" << data.m_ridlength
                  << " data.m_ridvalue:" << std::string(data.m_ridvalue.begin(), data.m_ridvalue.end()) << std::endl;
        //parseLwM2MObjects(std::string(contents.begin(), contents.end()), data, object);
        std::ostringstream rem;
        iss.get(*rem.rdbuf());
        object.m_value.push_back(data);
        data.clear();
        parseLwM2MObjects(rem.str(), data, object);

    } else if(typeValueOf76Bits == TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10) {
        
        switch(typeValueOf5thBit) {

            case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                {
                    /// One byte Resource Instance ID
                    if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_rid = static_cast<std::uint32_t>(onebyte);

                    break;
                }

            case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                {
                    /// Two Bytes are Resource Instance ID
                    std::uint16_t twobytes;
                    if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_rid = static_cast<std::uint32_t>(ntohs(twobytes));

                    break;
                }

            default:
                {
                    std::cout << basename(__FILE__) << ":" << " Error Resource Instance can't be greater than 2 bytes: " << std::to_string(typeValueOf5thBit) << std::endl;
                }
        }

        std::uint32_t len = 0;
        if(typeValueOf43Bits == TypeBits43_NoTypeLengthField_00) {

            len = static_cast<std::uint32_t>(typeValueOf20Bits);

        } else if(typeValueOf43Bits == TypeBits43_8BitsTypeLengthField_01) {

            if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
            }

            len = static_cast<std::uint32_t>(onebyte);

        } else if(typeValueOf43Bits == TypeBits43_16BitsTypeLengthField_10) {

            std::uint16_t twobytes;
            if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                return(-1);    
            }
            len = static_cast<std::uint32_t>(ntohs(twobytes));

        } else {

        }

        /// Read len number of bytes
        std::vector<std::uint8_t> contents(len);
        if(!iss.read(reinterpret_cast<char *>(contents.data()), contents.size()).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
        }
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");

        //data.m_ridlength = len;
        //data.m_ridvalue = contents;
        //object.m_value.push_back(data);
        //std::cout << basename(__FILE__) << ":" << __LINE__ << " data.m_riid:" << data.m_riid << " data.m_ridlength:" << data.m_ridlength 
        //          << " data.m_ridvalue:" << std::string(data.m_ridvalue.begin(), data.m_ridvalue.end()) << std::endl;
        //data.m_ridvalue.clear();

        std::ostringstream rem;
        iss.get(*rem.rdbuf());
        //std::string newcontents(contents.begin(), contents.end());
        parseLwM2MObjects(std::string(contents.begin(), contents.end()), data, object);
        parseLwM2MObjects(rem.str(), data, object);
        
    } else if(typeValueOf76Bits == TypeBits76_ResourceWithValue_11) {
        
        switch(typeValueOf5thBit) {

            case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                {
                    /// One byte identifier
                    if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_rid = static_cast<std::uint32_t>(onebyte);

                    break;
                }

            case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                {
                    /// Two Bytes are the identifier
                    std::uint16_t twobytes;
                    if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                        break;    
                    }
                    data.m_rid = static_cast<std::uint32_t>(ntohs(twobytes));

                    break;
                }

            default:
                {
                    std::cout << basename(__FILE__) << ":" << " Error identifier can't be greater than 2 bytes: " << std::to_string(typeValueOf5thBit) << std::endl;
                }
        }

        std::uint32_t len = 0;
        if(typeValueOf43Bits == TypeBits43_NoTypeLengthField_00) {

            len = static_cast<std::uint32_t>(typeValueOf20Bits);

        } else if(typeValueOf43Bits == TypeBits43_8BitsTypeLengthField_01) {

            if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
            }

            len = static_cast<std::uint32_t>(onebyte);

        } else if(typeValueOf43Bits == TypeBits43_16BitsTypeLengthField_10) {

            std::uint16_t twobytes;
            if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                return(-1);    
            }
            len = static_cast<std::uint32_t>(ntohs(twobytes));

        } else {

        }
        
        data.m_ridlength = len;

        /// Read len number of bytes
        std::vector<std::uint8_t> contents(len);
        if(!iss.read(reinterpret_cast<char *>(contents.data()), contents.size()).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
            return(0);
        }

        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");

        data.m_ridvalue = contents;
        
        std::cout << basename(__FILE__) << ":" << __LINE__ << " rid:" << std::to_string(data.m_rid) << " data.m_riid:" << data.m_riid  
                  << " length:" << std::to_string(data.m_ridlength) << " value:" << std::string(data.m_ridvalue.begin(), data.m_ridvalue.end()) << std::endl;

        object.m_value.push_back(data);
        data.clear();
        std::ostringstream rem;
        iss.get(*rem.rdbuf());
        parseLwM2MObjects(rem.str(), data, object);
    }
}

std::int32_t LwM2MAdapter::parseLwM2MPayload(const std::string& uri, const std::string& payload, std::vector<LwM2MObject>& objects) {

#if 0
    std::uint32_t oid = 0, oiid = 0, rid = 0;
    if(parseLwM2MUri(uri, oid, oiid, rid)) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Unable to parse URI: " << uri << std::endl;
        return(-1);
    }

    LwM2MObject object;
    TLV tlv;
    object.oid = oid;
    object.oiid = oiid;

    std::istringstream iss;
    iss.rdbuf()->pubsetbuf(const_cast<char *>(payload.data()), payload.length());
    std::uint8_t onebyte;

    do {
        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
            //std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
            break;    
        }

        std::uint8_t typeValueOf76Bits = (onebyte & 0b11000000) >> 6;
        std::uint8_t typeValueOf5thBit = (onebyte & 0b00100000) >> 5;
        std::uint8_t typeValueOf43Bits = (onebyte & 0b00011000) >> 3;
        std::uint8_t typeValueOf20Bits = (onebyte & 0b00000111) >> 0;

        //std::cout << basename(__FILE__) << ":" << __LINE__ << " typeValueOf76Bits:" << std::to_string(typeValueOf76Bits) << " typeValueOf5thBit:" << std::to_string(typeValueOf5thBit)
        //          << " typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << " typeValueOf20Bits:" << std::to_string(typeValueOf20Bits) << std::endl;

        switch(typeValueOf76Bits) {
            case TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00:
            {
                /// The Value part of this TLV is another TLV resource.
                switch(typeValueOf5thBit) {
                    case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                    {
                        /// One byte length
                        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                            break;    
                        }
                        object.oiid = static_cast<std::uint32_t>(onebyte);

                        break;
                    }
                    case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                    {
                        std::uint16_t twobytes;
                        /// Two Bytes Length
                        if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                            break;    
                        }
                        object.oiid = static_cast<std::uint32_t>(ntohs(twobytes));

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
                        TLV tlv;
                        tlv.m_length = typeValueOf20Bits;
                        std::vector<std::uint8_t> dd(tlv.m_length);
                        if(!iss.read(reinterpret_cast<char *>(dd.data()), dd.size()).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " Error input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_value = dd;

                        object.tlvs.push_back(tlv);
                        objects.push_back(object);
                        break;
                    }

                    case TypeBits43_8BitsTypeLengthField_01:
                    {
                        ///The length of Type filed is one Byte in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        std::vector<std::uint8_t> contents(onebyte);
                        if(!iss.read(reinterpret_cast<char *>(contents.data()), onebyte).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        
                        std::string newcontents(contents.begin(), contents.end());
                        iss.rdbuf()->pubsetbuf(const_cast<char *>(newcontents.data()), newcontents.length());

                        break;
                    }

                    case TypeBits43_16BitsTypeLengthField_10:
                    {
                        std::uint16_t twobytes;
                        ///The length of Type filed is Two Bytes in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&twobytes), sizeof(twobytes)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        twobytes = ntohs(twobytes);
                        std::vector<std::uint8_t> contents(56652);
                        if(!iss.read(reinterpret_cast<char *>(contents.data()), onebyte).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        contents.resize(iss.gcount());
                        std::string newcontents(contents.begin(), contents.end());
                        iss.rdbuf()->pubsetbuf(const_cast<char *>(newcontents.data()), newcontents.length());

                        break;
                    }
                    case TypeBits43_24BitsTypeLengthField_11:
                    {
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Not supported of 3 bytes length yet" << std::endl;
                        break;
                    }
                    default:
                    {
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << std::endl;
                    }
                }
                break;
            }
                
            case TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01:
            {
                tlv.m_type = TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01;
                if(!typeValueOf5thBit && !typeValueOf43Bits) {
                    tlv.m_length = typeValueOf20Bits;
                    /// One byte length
                    if(!iss.read(reinterpret_cast<char *>(&object.rid), 1/*One Byte*/).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                        break;    
                    }

                    if(!iss.read(reinterpret_cast<char *>(tlv.m_value.data()), tlv.m_length).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                        //tlv.m_value.resize(iss.gcount());
                        //tlv().push_back(tlv);
                        break;    
                    }
                    tlv.m_value.resize(iss.gcount());

                    object.tlvs.push_back(tlv);
                    objects.push_back(object);
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_length:" << tlv.m_length << std::endl;
                }

                break;
            }
            case TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10:
            {
                tlv.m_type = TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10;

                if(!typeValueOf5thBit) {

                    if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                        break;    
                    }

                    tlv.m_identifier = onebyte;
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_identifier:" <<  std::hex << tlv.m_identifier << std::dec << std::endl;

                    if(!typeValueOf43Bits) {
                        tlv.m_length = static_cast<std::uint32_t>(typeValueOf20Bits);
                    } else {
                        if(!iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_length = static_cast<std::uint32_t>(onebyte);
                    }

                    std::vector<std::uint8_t> dd(tlv.m_length);
                    if(!iss.read(reinterpret_cast<char *>(dd.data()), dd.size()).good()) {
                        std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                        //tlv.m_value.resize(iss.gcount());
                        //tlv().push_back(tlv);
                        break;    
                    }
                    tlv.m_value = dd;

                    object.tlvs.push_back(tlv);
                    objects.push_back(object);
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_length:" <<  std::hex << tlv.m_length << std::dec << std::endl;

                    std::string multiplerid(std::string(tlv.m_value.begin(), tlv.m_value.end()));
                    for(const auto& ent: tlv.m_value) {
                        printf("%0.2X ", static_cast<std::uint32_t>(ent));
                    }
                    std::cout << std::endl;
                    std::istringstream newiss;
                    
                    newiss.rdbuf()->pubsetbuf(const_cast<char *>(multiplerid.data()), multiplerid.length());
                    while(1) {
                        
                        if(!newiss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                            //std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " value of onebyte: 0x" << std::hex << onebyte << std::dec << std::endl;

                        std::uint8_t typeValueOf76Bits = (onebyte & 0b11000000) >> 6;
                        std::uint8_t typeValueOf5thBit = (onebyte & 0b00100000) >> 5;
                        std::uint8_t typeValueOf43Bits = (onebyte & 0b00011000) >> 3;
                        std::uint8_t typeValueOf20Bits = (onebyte & 0b00000111) >> 0;

                        std::cout << basename(__FILE__) << ":" << __LINE__ << " typeValueOf76Bits:" << std::to_string(typeValueOf76Bits) << " typeValueOf5thBit:" << std::to_string(typeValueOf5thBit)
                                << " typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << " typeValueOf20Bits:" << std::to_string(typeValueOf20Bits) << std::endl;
                        
                        LwM2MObject newobject;

                        if(!typeValueOf5thBit) {
                            if(!newiss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                                std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                                break;    
                            }

                            /// This is resource Instance ID
                            
                            newobject.oid = object.oid;
                            newobject.oiid = object.oiid;
                            newobject.rid == static_cast<std::uint32_t>(onebyte);
                            std::cout << basename(__FILE__) << ":" << __LINE__ << " rid: 0x" << std::hex << newobject.rid << std::dec << std::endl;
                        }

                        if(!typeValueOf43Bits) {
                            TLV tlv;
                            tlv.m_length = static_cast<std::uint32_t>(typeValueOf20Bits);
                            std::vector<std::uint8_t> dd(tlv.m_length);
                            if(!newiss.read(reinterpret_cast<char *>(dd.data()), dd.size()).good()) {
                                std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                                break;    
                            }

                            tlv.m_identifier = onebyte;
                            tlv.m_value = dd;
                            newobject.tlvs.push_back(tlv);
                            objects.push_back(newobject);
                        }
                    }
                }
                
                break;
            }

            case TypeBits76_ResourceWithValue_11:
            {
                tlv.m_type = TypeBits76_ResourceWithValue_11;

                switch(typeValueOf5thBit) {
                    case TypeBit5_LengthOfTheIdentifier8BitsLong_0:
                    {
                        /// One byte length
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_identifier), 1/*One Byte*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        
                        tlv.m_identifier = static_cast<std::uint32_t>(tlv.m_identifier);
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_identifier:" << tlv.m_identifier << std::endl;
                        break;
                    }
                    case TypeBit5_LengthOfTheIdentifier16BitsLong_1:
                    {
                        /// Two Bytes Length
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_identifier), 2/*Two Bytes*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_identifier = static_cast<std::uint32_t>(tlv.m_identifier);

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
                        tlv.m_length = typeValueOf20Bits;
                        std::vector<std::uint8_t> dd(tlv.m_length);
                        if(!iss.read(reinterpret_cast<char *>(dd.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            //tlv.m_value.resize(iss.gcount());
                            //tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value = dd;

                        object.tlvs.push_back(tlv);
                        objects.push_back(object);
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_length:" << tlv.m_length << std::endl;
                        break;
                    }
                    case TypeBits43_8BitsTypeLengthField_01:
                    {
                        ///The length of Type filed is one Byte in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_length), 1/*Byte*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }

                        std::cout << basename(__FILE__) << ":" << __LINE__ << " tlv.m_length:" << tlv.m_length << std::endl; 
                        std::vector<std::uint8_t> dd(tlv.m_length);
                        if(!iss.read(reinterpret_cast<char *>(dd.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            //tlv.m_value.resize(iss.gcount());
                            //tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value = dd;

                        object.tlvs.push_back(tlv);
                        objects.push_back(object);

                        break;
                    }
                    case TypeBits43_16BitsTypeLengthField_10:
                    {
                        ///The length of Type filed is Two Bytes in encoded TLV
                        if(!iss.read(reinterpret_cast<char *>(&tlv.m_length), 2/*Two Bytes*/).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            break;    
                        }
                        tlv.m_length = static_cast<std::uint32_t>(ntohs(tlv.m_length));

                        if(!iss.read(reinterpret_cast<char *>(tlv.m_value.data()), tlv.m_length).good()) {
                            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                            //tlv.m_value.resize(iss.gcount());
                            //tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value.resize(iss.gcount());

                        object.tlvs.push_back(tlv);
                        objects.push_back(object);

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
                            //tlv.m_value.resize(iss.gcount());
                            //tlv().push_back(tlv);
                            break;    
                        }
                        tlv.m_value.resize(iss.gcount());

                        object.tlvs.push_back(tlv);
                        objects.push_back(object);

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
            {

            }

        }

    } while(1);
#endif
}

std::int32_t LwM2MAdapter::buildLwM2MPayload(const std::string& oid, const std::string& oiid, const std::string& orid, std::vector<LwM2MObject>& objects) {

}






#endif /*__lwm2m_adapter_cpp__*/