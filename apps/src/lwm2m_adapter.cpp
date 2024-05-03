#ifndef __lwm2m_adapter_cpp__
#define __lwm2m_adapter_cpp__

#include "lwm2m_adapter.hpp"

LwM2MAdapter::LwM2MAdapter(CoAPAdapter& coapAdapter) : m_coapAdapter(coapAdapter) {
    ///LwM2M Security Object URI --> /0
    SecurityObjectResourceId2ResourceName = {
        {0, "LwM2M Server URI"},
        {1, "Bootstrap Server"},
        {2, "Security Mode"},
        {3, "Public Key or Identity"},
        {4, "Server Public Key"},
        {5, "Secret Key"},
        {6, "SMS Security Mode"},
        {7, "SMS Binding Key Parameters"},
        {8, "SMS Binding Secret Key(s)"},
        {9, "LwM2M Server SMS Number"},
        {10, "Short Server ID"},
        {11, "Client Hold Off Time"},
        {12, "Bootstrap-Server Account Timeout"}
    };

    ///LwM2M Security Object URI --> /0
    ResourceName2SecurityObjectResourceId = {
        {"LwM2M Server URI", 0},
        {"Bootstrap Server", 1},
        {"Security Mode", 2},
        {"Public Key or Identity", 3},
        {"Server Public Key", 4},
        {"Secret Key", 5},
        {"SMS Security Mode", 6},
        {"SMS Binding Key Parameters", 7},
        {"SMS Binding Secret Key(s)", 8},
        {"LwM2M Server SMS Number", 9},
        {"Short Server ID", 10},
        {"Client Hold Off Time", 11},
        {"Bootstrap-Server Account Timeout", 12}
    };

    ///LwM2M Server Object URI --> /1
    ServerObjectResourceId2ResourceName = {
        {0, "Short Server ID"},
        {1, "Lifetime"},
        {2, "Default Minimum Period"},
        {3, "Default Maximum Period"},
        {4, "Disable"},
        {5, "Disable Timeout"},
        {6, "Notification Storing When Disabled or Offline"},
        {7, "Binding"},
        {8, "Registration Update Trigger"}
    };

    ///LwM2M Server Object URI --> /1
    ResourceName2ServerObjectResourceId = {
        {"Short Server ID", 0},
        {"Lifetime", 1},
        {"Default Minimum Period", 2},
        {"Default Maximum Period", 3},
        {"Disable", 4},
        {"Disable Timeout", 5},
        {"Notification Storing When Disabled or Offline", 6},
        {"Binding", 7},
        {"Registration Update Trigger", 8}
    };

    ///LwM2M Acces Control Object URI --> /2
    AccessControlObjectResourceId2ResourceName = {
        {0, "Object ID"},
        {1, "Object Instance ID"},
        {2, "ACL"},
        {3, "Access Control Owner"}
    };

    ///LwM2M Acces Control Object URI --> /2
    ResourceName2AccessControlObjectResourceId = {
        {"Object ID", 0},
        {"Object Instance ID", 1},
        {"ACL", 2},
        {"Access Control Owner", 3}
    };

    ///LwM2M Device Object URI --> /3
    DeviceObjectResourceId2ResourceName = {
        {0, "Manufacturer"},
        {1, "Model Number"},
        {2, "Serial Number"},
        {3, "Firmware Version"},
        {4, "Reboot"},
        {5, "Factory Reset"},
        {6, "Available Power Sources"},
        {7, "Power Source Voltage"},
        {8, "Power Source Current"},
        {9, "Battery Level"},
        {10, "Memory Free"},
        {11, "Error Code"},
        {12, "Reset Error Code"},
        {13, "Current Time"},
        {14, "UTC Offset"},
        {15, "Timezone"},
        {16, "Supported Binding and Modes"},
        {17, "Device Type"},
        {18, "Hardware Version"},
        {19, "Software Version"},
        {20, "Battery Status"},
        {21, "Memory Total"},
        {22, "ExtDevInfo"}

    };

    ///LwM2M Device Object URI --> /3
    ResourceName2DeviceObjectResourceId = {
        {"Manufacturer", 0},
        {"Model Number", 1},
        {"Serial Number", 2},
        {"Firmware Version", 3},
        {"Reboot", 4},
        {"Factory Reset", 5},
        {"Available Power Sources", 6},
        {"Power Source Voltage", 7},
        {"Power Source Current", 8},
        {"Battery Level", 9},
        {"Memory Free", 10},
        {"Error Code", 11},
        {"Reset Error Code", 12},
        {"Current Time", 13},
        {"UTC Offset", 14},
        {"Timezone", 15},
        {"Supported Binding and Modes", 16},
        {"Device Type", 17},
        {"Hardware Version", 18},
        {"Software Version", 19},
        {"Battery Status", 20},
        {"Memory Total", 21},
        {"ExtDevInfo", 22}
    };
    ///LwM2M Connectivity Monitoring Object URI --> /4
    ConnectivityMonitoringObjectResourceId2ResourceName = {
        {0, "Network Bearer"},
        {1, "Available Network Bearer"},
        {2, "Radio Signal Strength"},
        {3, "Link Quality"},
        {4, "IP Addresses"},
        {5, "Router IP Addresses"},
        {6, "Link Utilization"},
        {7, "APN"},
        {8, "Cell ID"},
        {9, "SMNC"},
        {10, "SMCC"}
    };
    ///LwM2M Connectivity Monitoring Object URI --> /4
    ResourceName2ConnectivityMonitoringObjectResourceId = {
        {"Network Bearer", 0},
        {"Available Network Bearer", 1},
        {"Radio Signal Strength", 2},
        {"Link Quality", 3},
        {"IP Addresses", 4},
        {"Router IP Addresses", 5},
        {"Link Utilization", 6},
        {"APN", 7},
        {"Cell ID", 8},
        {"SMNC", 9},
        {"SMCC", 10}
    };
    ///LwM2M Firmware Update Object URI --> /5
    FirmwareUpdateObjectResourceId2ResourceName = {
        {0, "Package"},
        {1, "Package URI"},
        {2, "Update"},
        {3, "State"},
        {5, "Update Result"},
        {6, "PkgName"},
        {7, "PkgVersion"},
        {8, "Firmware Update Protocol Support"},
        {9, "Firmware Update Delivery Method"}

    };

    ///LwM2M Firmware Update Object URI --> /5
    ResourceName2FirmwareUpdateObjectResourceId = {
        {"Package", 0},
        {"Package URI", 1},
        {"Update", 2},
        {"State", 3},
        {"Update Result", 5},
        {"PkgName", 6},
        {"PkgVersion", 7},
        {"Firmware Update Protocol Support", 8},
        {"Firmware Update Delivery Method", 9}
    };
    ///LwM2M Location Object URI --> /6
    LocationObjectResourceId2ResourceName = {
        {0, "Latitude"},
        {1, "Longitude"},
        {2, "Altitude"},
        {3, "Radius"},
        {4, "Velocity"},
        {5, "Timestamp"},
        {6, "Speed"}

    };

    ///LwM2M Location Object URI --> /6
    ResourceName2LocationObjectResourceId = {
        {"Latitude", 0},
        {"Longitude", 1},
        {"Altitude", 2},
        {"Radius", 3},
        {"Velocity", 4},
        {"Timestamp", 5},
        {"Speed", 6}
    };

    ///LwM2M Connectivity Stats Object URI --> /7
    ConnectivityStatsObjectResourceId2ResourceName = {
        {0, "SMS Tx Counter"},
        {1, "SMS Rx Counter"},
        {2, "Tx Data"},
        {3, "Rx Data"},
        {4, "Max Message Size"},
        {5, "Average Message Size"},
        {6, "Start"},
        {7, "Stop"},
        {8, "Collection Period"}

    };

    ///LwM2M Connectivity Stats Object URI --> /7
    ResourceName2ConnectivityStatsObjectResourceId = {
        {"SMS Tx Counter", 0},
        {"SMS Rx Counter", 1},
        {"Tx Data", 2},
        {"Rx Data", 3},
        {"Max Message Size", 4},
        {"Average Message Size", 5},
        {"Start", 6},
        {"Stop", 7},
        {"Collection Period", 8}
    };
}

LwM2MAdapter::~LwM2MAdapter() {

}

std::string LwM2MAdapter::resourceIDName(const std::uint32_t& oid, const std::uint32_t& rid) {
    std::string out;

    switch(oid) {
        case SecurityObjectID:
        {
            auto it = std::find_if(SecurityObjectResourceId2ResourceName.begin(), SecurityObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != SecurityObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case ServerObjectID:
        {
            auto it = std::find_if(ServerObjectResourceId2ResourceName.begin(), ServerObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != ServerObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case AccessControlObjectID:
        {
            auto it = std::find_if(AccessControlObjectResourceId2ResourceName.begin(), AccessControlObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != AccessControlObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case DeviceObjectID:
        {
            auto it = std::find_if(DeviceObjectResourceId2ResourceName.begin(), DeviceObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != DeviceObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case ConnectivityMonitoringObjectID:
        {
            auto it = std::find_if(ConnectivityMonitoringObjectResourceId2ResourceName.begin(), ConnectivityMonitoringObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != ConnectivityMonitoringObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case FirmwareUpdateObjectID:
        {
            auto it = std::find_if(FirmwareUpdateObjectResourceId2ResourceName.begin(), FirmwareUpdateObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != FirmwareUpdateObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case LocationObjectID:
        {
            auto it = std::find_if(LocationObjectResourceId2ResourceName.begin(), LocationObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != LocationObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        case ConnectivityStatisticsObjectID:
        {
            auto it = std::find_if(ConnectivityStatsObjectResourceId2ResourceName.begin(), ConnectivityStatsObjectResourceId2ResourceName.end(), [&](const auto& ent) -> bool {
                return(ent.first == rid);
            });

            if(it != ConnectivityStatsObjectResourceId2ResourceName.end()) {
                out.assign(it->second);
            }

            break;
        }
        default:
        {

        }
    }
    return(out);
}

std::int32_t LwM2MAdapter::parseLwM2MUri(const std::string& uri, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid) {

    if(uri.empty() || (uri.at(0) != '/')) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error uri is empty" << std::endl;
        return(-1);
    }

    std::istringstream iss;
    char delim = '/';
    std::ostringstream value;
    std::vector<std::uint32_t> objects;
    iss.rdbuf()->pubsetbuf(const_cast<char *>(uri.data()), uri.length());
    iss.get();

    while(iss.get(*value.rdbuf(), delim).good()) {
        if(value.str().length()) {
            objects.push_back(std::stoi(value.str()));
            ///uri starts with '/'
            iss.get();
        }
        value.str("");
    }

    if(iss.eof() && iss.gcount() > 0) {
        objects.push_back(std::stoi(value.str()));
    }

    if(objects.size() > 0)
        oid = objects.at(0);
    if(objects.size() > 1)
        oiid = objects.at(1);
    if(objects.size() > 2)
        rid = objects.at(2);
    
    return(0);
}

std::int32_t LwM2MAdapter::parseLwM2MObjects(const std::string& payload, LwM2MObjectData& data, LwM2MObject& object) {
    std::istringstream iss;
    iss.rdbuf()->pubsetbuf(const_cast<char *>(payload.data()), payload.length());
    std::uint8_t onebyte;

    if(iss.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).eof()) {
        return(0);
    }

    std::uint8_t typeValueOf76Bits = (onebyte & 0b11000000) >> 6;
    std::uint8_t typeValueOf5thBit = (onebyte & 0b00100000) >> 5;
    std::uint8_t typeValueOf43Bits = (onebyte & 0b00011000) >> 3;
    std::uint8_t typeValueOf20Bits = (onebyte & 0b00000111) >> 0;
#ifdef __DEBUG__
    std::cout << basename(__FILE__) << ":" << __LINE__ << " typeValueOf76Bits:" << std::to_string(typeValueOf76Bits) << " typeValueOf5thBit:" << std::to_string(typeValueOf5thBit)
              << " typeValueOf43Bits:" << std::to_string(typeValueOf43Bits) << " typeValueOf20Bits:" << std::to_string(typeValueOf20Bits) << std::endl;
#endif
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
#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");
#endif
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

#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");
#endif
        data.m_ridlength = len;
        data.m_ridvalue.resize(len);
        data.m_ridvalue = contents;
#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " data.m_riid:" << data.m_riid << " data.m_rid:" << data.m_rid << " data.m_ridlength:" << data.m_ridlength
                  << " data.m_ridvalue:" << std::string(data.m_ridvalue.begin(), data.m_ridvalue.end()) << std::endl;
        //parseLwM2MObjects(std::string(contents.begin(), contents.end()), data, object);
#endif
        std::ostringstream rem;
        iss.get(*rem.rdbuf(), EOF);
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
#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");
#endif
        std::ostringstream rem;
        iss.get(*rem.rdbuf(), EOF);
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
#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: contents) {
            printf("%0.2X ", ent);
        }
        printf("\n");
#endif
        data.m_ridvalue = contents;

#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " rid:" << std::to_string(data.m_rid) << " data.m_riid:" << data.m_riid  
                  << " length:" << std::to_string(data.m_ridlength) << " value:" << std::string(data.m_ridvalue.begin(), data.m_ridvalue.end()) << std::endl;
#endif
        object.m_value.push_back(data);
        data.clear();
        std::ostringstream rem;
        iss.get(*rem.rdbuf(), EOF);
#ifdef __DEBUG__
        std::cout << basename(__FILE__) << ":" << __LINE__ << " ";
        for(const auto& ent: rem.str()) {
            printf("%0.2X ", static_cast<std::uint8_t>(ent));
        }
        printf("\n");
#endif
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

std::int32_t LwM2MAdapter::serialiseTLV(const TypeFieldOfTLV_t& bits76, std::string value, std::uint16_t id, std::string& out) {
    std::uint8_t type;
    std::stringstream ss;

    /// id could be any of these --- The Object Instance, Resource, or Resource Instance ID as indicated by the Type field
    type = bits76 << 6;

    if(id >=0 && id <= 255) {
        type |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
    } else {
        type |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
    }

    if(value.length() >= 0 && value.length() < 8) {

        type |= TypeBits43_NoTypeLengthField_00 << 3;
        type |= value.length() & 0b111;

    } else if(value.length() > 0 && value.length() <= 255) {

        type |= TypeBits43_8BitsTypeLengthField_01 << 3;
        type |= value.length() & 0b000;

    } else if(value.length() > 255 && value.length() <= 65535) {

        type |= TypeBits43_16BitsTypeLengthField_10 << 3;
        type |= value.length() & 0b000;

    } else {
        ///length is24 bits
    }

    ss.write(reinterpret_cast<char*>(&type), sizeof(type));
    if(id >= 0 && id <=255) {
        ss.write(reinterpret_cast<char*>(&id), 1);
    } else {
        std::uint16_t tmpid = htons(id);
        ss.write(reinterpret_cast<char*>(&tmpid), sizeof(tmpid));
    }

    if(value.length() >= 0 && value.length() < 8) {

        ss.write(reinterpret_cast<char*>(value.data()), value.length());

    } else if(value.length() > 0 && value.length() <= 255) {

        std::uint8_t len = value.length();
        ss.write(reinterpret_cast<char*>(&len), sizeof(len));
        ss.write(reinterpret_cast<char*>(value.data()), value.length());

    } else if(value.length() > 255 && value.length() <= 65535) {

        std::uint16_t len = value.length();
        ss.write(reinterpret_cast<char*>(&len), sizeof(len));
        ss.write(reinterpret_cast<char*>(value.data()), value.length());

    } else {
        ///length is24 bits
    }
    
    out.assign(ss.str());
    return(0);
}

std::int32_t LwM2MAdapter::serialiseTLV(const TypeFieldOfTLV_t& bits76, std::uint32_t value, std::uint16_t id, std::string& out) {
    std::uint8_t type = 0;
    std::stringstream ss;

    /// id could be any of these --- The Object Instance, Resource, or Resource Instance ID as indicated by the Type field
    type = bits76 << 6;

    if(id >=0 && id <= 255) {
        type |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
    } else {
        type |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
    }

    if(value >= 0 && value < 8) {

        type |= TypeBits43_NoTypeLengthField_00 << 3;
        type |= 1 & 0b111;

    } else if(value > 0 && value <= 255) {

        type |= TypeBits43_NoTypeLengthField_00 << 3;
        type |= 1 & 0b111;

    } else if(value > 255 && value <= 65535) {

        type |= TypeBits43_NoTypeLengthField_00 << 3;
        type |= 2 & 0b111;

    } else {

        type |= TypeBits43_NoTypeLengthField_00 << 3;
        type |= 4 & 0b111;
    }

    ss.write(reinterpret_cast<char*>(&type), sizeof(type));
    if(id >= 0 && id <=255) {
        ss.write(reinterpret_cast<char*>(&id), 1);
    } else {
        std::uint16_t tmpid = htons(id);
        ss.write(reinterpret_cast<char*>(&tmpid), 2);
    }

    if(value >= 0 && value < 8) {

        ss.write(reinterpret_cast<char*>(&value), 1);

    } else if(value > 0 && value <= 255) {

        ss.write(reinterpret_cast<char*>(&value), 1);

    } else if(value > 255 && value <= 65535) {

        std::uint16_t tmpvalue = htons(value);
        ss.write(reinterpret_cast<char*>(&tmpvalue), sizeof(tmpvalue));

    } else {

        std::uint32_t tmpvalue = htonl(value);
        ss.write(reinterpret_cast<char*>(&tmpvalue), sizeof(tmpvalue));
    }
    
    out.assign(ss.str());
    return(0);
}

std::int32_t LwM2MAdapter::serialiseTLV(const TypeFieldOfTLV_t& bits76, bool value, std::uint16_t id, std::string& out) {
    std::uint8_t type;
    std::stringstream ss;

    /// id could be any of these --- The Object Instance, Resource, or Resource Instance ID as indicated by the Type field
    type = bits76 << 6;

    if(id >=0 && id <= 255) {
        type |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
    } else {
        type |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
    }

    type |= TypeBits43_NoTypeLengthField_00 << 3;
    type |= 1 & 0b111;

    ss.write(reinterpret_cast<char*>(&type), sizeof(type));
    if(id >= 0 && id <= 255) {
        ss.write(reinterpret_cast<char*>(&id), 1);
    } else {
        std::uint16_t tmpid = htons(id);
        ss.write(reinterpret_cast<char*>(&tmpid), sizeof(tmpid));
    }

    ss.write(reinterpret_cast<char*>(&value), 1);
    out.assign(ss.str());
    return(0);
}

std::int32_t LwM2MAdapter::serialiseObjects(const json& rid, std::string& out) {
    std::uint16_t identifier = rid["rid"].get<std::uint16_t>();
    std::stringstream ss, tmpss;

    if(rid["value"].is_array()) {

        std::uint16_t riid = 0;
        tmpss.str("");

        for(const auto& ent: rid["value"]) {
            std::string out;
            if(ent.is_string()) {

                serialiseTLV(TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01, ent.get<std::string>(), riid, out);
                tmpss.write(reinterpret_cast<char *>(out.data()), out.length());
                            
            } else if(ent.is_boolean()) {

                serialiseTLV(TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01, ent.get<bool>(), riid, out);
                tmpss.write(reinterpret_cast<char *>(out.data()), out.length());

            } else if(ent.is_number()) {

                serialiseTLV(TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01, ent.get<std::uint32_t>(), riid, out);
                tmpss.write(reinterpret_cast<char *>(out.data()), out.length());

            } else if(rid["value"].is_binary()) {
                ///@this must either be identity or secret
                auto sz = rid["value"].get_binary().size();
                auto data = rid["value"].get_binary();

                std::string st(reinterpret_cast<char *>(data.data()), sz);
                serialiseTLV(TypeBits76_ResourceWithValue_11, st, identifier, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            } else {
                std::cout << basename(__FILE__) << ":" << __LINE__ << " unsupported type" << std::endl;
            }
            riid++;
        }
        serialiseTLV(TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10, tmpss.str(), identifier, out);
        ss.write(reinterpret_cast<char *>(out.data()), out.length());

    } else if(rid["value"].is_string()) {

        serialiseTLV(TypeBits76_ResourceWithValue_11, rid["value"].get<std::string>(), identifier, out);
        ss.write(reinterpret_cast<char *>(out.data()), out.length());

    } else if(rid["value"].is_boolean()) {

        serialiseTLV(TypeBits76_ResourceWithValue_11, rid["value"].get<bool>(), identifier, out);
        ss.write(reinterpret_cast<char *>(out.data()), out.length());

    } else if(rid["value"].is_number()) {

        serialiseTLV(TypeBits76_ResourceWithValue_11, rid["value"].get<std::uint32_t>(), identifier, out);
        ss.write(reinterpret_cast<char *>(out.data()), out.length());

    } else if(rid["value"].is_binary()) {
        ///@this must either be identity or secret
        auto sz = rid["value"].get_binary().size();
        auto data = rid["value"].get_binary();

        std::string st(reinterpret_cast<char *>(data.data()), sz);
        serialiseTLV(TypeBits76_ResourceWithValue_11, st, identifier, out);
        ss.write(reinterpret_cast<char *>(out.data()), out.length());

    } else {

    }

    out.assign(ss.str());
    return(0);
}

std::int32_t LwM2MAdapter::buildLwM2MPayload(const ObjectId_t& oid, std::string oiid, json& rids, std::string& out) {
    
    switch (oid)
    {
    case SecurityObjectID:
    {
        std::stringstream ss;

        if(oiid.length()) {

            if(oiid == "0") {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }
            } else {
                for(auto& rid: rids) {

                    if(rid["rid"].is_number() && (rid["rid"].get<std::uint32_t>() == 3/*For identity*/)) {
                        ///@brief identity - 128 bits
                        std::vector<std::uint8_t> iden(16);
                        std::ifstream ifs("/dev/urandom");

                        if(ifs.is_open()) {
                            if(ifs.read(reinterpret_cast<char *>(iden.data()), iden.size()).good()) {
                                ///@brief 
                                rid["value"] = json::binary(iden, iden.size());
                                //rid["value"] = std::string(iden.begin(), iden.end());
                            }
                            ifs.close();
                        }

                    } else if(rid["rid"].is_number() && (rid["rid"].get<std::uint32_t>() == 5/*For PSK*/)) {
                        ///@brief secret - 128 bits
                        std::vector<std::uint8_t> sec(16);
                        std::ifstream ifs("/dev/urandom");

                        if(ifs.is_open()) {
                            if(ifs.read(reinterpret_cast<char *>(sec.data()), sec.size()).good()) {
                                ///@brief convert base64
                                rid["value"] = json::binary(sec, 16);
                                //rid["value"] = std::string(sec.begin(), sec.end());
                            }
                            ifs.close();
                        }
                    }

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }
            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;
    case ServerObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {

            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    case AccessControlObjectID:
        /* code */
    {
        std::stringstream ss;

        if(oiid.length()) {

            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;
    
    case DeviceObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {

            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    case ConnectivityMonitoringObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {

            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    case FirmwareUpdateObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {

            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    case LocationObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {
            
            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    case ConnectivityStatisticsObjectID:
    {
        /* code */
        std::stringstream ss;

        if(oiid.length()) {
            
            for(const auto& rid: rids) {

                serialiseObjects(rid, out);
                ss.write(reinterpret_cast<char *>(out.data()), out.length());

            }
            out.assign(ss.str());

        } else {
            /// no object instance id in the uri
            std::stringstream newss;
            for(const  auto& ent: {0, 1}) {

                for(const auto& rid: rids) {

                    serialiseObjects(rid, out);
                    ss.write(reinterpret_cast<char *>(out.data()), out.length());

                }

                serialiseTLV(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00, ss.str(), ent, out);
                newss.write(reinterpret_cast<char *>(out.data()), out.length());
                ss.str("");

            }
            out.assign(newss.str());
        }
    }
        break;

    default:
        break;
    }
}


std::int32_t LwM2MAdapter::bootstrapSecurityObject00(std::string& out) {
    std::ifstream ifs("../config/securityObject/0.json");
    std::stringstream ss("");

    if(ifs.is_open()) {
        ss << ifs.rdbuf();
        ifs.close();

        json LwM2MObject00 = json::parse(ss.str());
        buildLwM2MPayload(SecurityObjectID, std::to_string(0), LwM2MObject00, out);
        return(0);
    }
    return(-1);

}

std::int32_t LwM2MAdapter::devicemgmtSecurityObject01(std::string& out) {
    std::ifstream ifs("../config/securityObject/1.json");
    std::stringstream ss("");

    if(ifs.is_open()) {
        ss << ifs.rdbuf();
        ifs.close();

        json LwM2MObject01 = json::parse(ss.str());
        buildLwM2MPayload(SecurityObjectID, std::to_string(1), LwM2MObject01, out);
        std::ofstream ofs("../config/securityObject/modified1.json");

        if(ofs.is_open()) {
            std::string out = LwM2MObject01.dump();
            ofs.write(out.c_str(), out.length());
        }

        ofs.close();

        return(0);
    }
    return(-1);
}

std::int32_t LwM2MAdapter::serverObject30(std::string& out) {
    std::ifstream ifs("../config/serverObject/0.json");
    std::stringstream ss("");

    if(ifs.is_open()) {
        ss << ifs.rdbuf();
        ifs.close();

        json LwM2MObject30 = json::parse(ss.str());
        buildLwM2MPayload(ServerObjectID, std::to_string(0), LwM2MObject30, out);
        return(0);
    }
    return(-1);
}

std::int32_t LwM2MAdapter::securityObject(std::string& out) {
    std::string object;
    std::stringstream ss;
    if(!bootstrapSecurityObject00(object)) {
        ss.write(reinterpret_cast<char *>(object.data()), object.length());
        
        if(!devicemgmtSecurityObject01(object)) {
            ss.write(reinterpret_cast<char *>(object.data()), object.length());
        }

        out.assign(ss.str());
        return(0);
    }
    return(-1);

}

#endif /*__lwm2m_adapter_cpp__*/