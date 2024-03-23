#ifndef __lwm2m_adapter_cpp__
#define __lwm2m_adapter_cpp__

#include "lwm2m_adapter.hpp"

LwM2MAdapter::LwM2MAdapter() {
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

std::int32_t LwM2MAdapter::parseLwM2MUri(const std::string&& uri, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid) {

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

std::int32_t LwM2MAdapter::buildLwM2MPayload(const ObjectId_t& oid, const std::string& oiid, const json& rids, std::string& out) {
    
    switch (oid)
    {
    case SecurityObjectID:
    {
        std::stringstream ss;
        std::uint8_t type;
        std::uint16_t len;

        if(oiid.length()) {
            for(const auto& rid: rids) {

                std::uint8_t type;
                std::uint16_t identifier = rid["rid"].get<std::uint16_t>();
                std::uint16_t length;
                std::string value;

                if(rid["value"].is_array()) {

                    std::uint16_t riid = 0;
                    std::stringstream riss;
                    std::uint16_t rilength;
                    std::string rivalue;
                    std::uint8_t ritype;

                    type = TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10 << 6;

                    for(const auto& ent: rid["value"]) {

                        if(ent.is_string()) {

                            rivalue.assign(ent.get<std::string>());
                            rilength = rivalue.length();
                            
                        } else if(ent.is_boolean()) {
                            
                        } else if(ent.is_number()) {

                        } else {
                            std::cout << basename(__FILE__) << ":" << __LINE__ << " unsupported type" << std::endl;
                        }

                        ritype = TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01 << 6;
                        if(riid < 256 && riid >= 0) {
                            ritype |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
                        } else {
                            ritype |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
                        }

                        if(rilength >= 0 && rilength < 8) {

                            ritype |= TypeBits43_NoTypeLengthField_00 << 3;
                            ritype |= rilength & 0b111;

                        } else if(rilength > 0 && rilength < 256) {

                            ritype |= TypeBits43_8BitsTypeLengthField_01 << 3;
                            ritype |= rilength & 0b000;

                        } else if(rilength > 255 && rilength <= 65535) {

                            ritype |= TypeBits43_16BitsTypeLengthField_10 << 3;
                            ritype |= length & 0b000;

                        } else {
                            ///length is24 bits
                        }

                        riss.write(reinterpret_cast<char*>(&ritype), sizeof(ritype));
                        if(rilength > 7 && rilength < 256) {

                            riss.write(reinterpret_cast<char*>(&riid), 1);
                            riss.write(reinterpret_cast<char*>(&rilength), 1);

                        } else if(rilength > 255 && rilength <= 65535) {

                            //ss.write(reinterpret_cast<char*>(&riid), sizeof(riid));
                            std::uint16_t tmplen = htons(rilength);
                            riss.write(reinterpret_cast<char*>(&tmplen), sizeof(tmplen));

                        }

                        riss.write(reinterpret_cast<char*>(rivalue.data()), rivalue.length());
                        riid++;
                    }

                    len = riss.str().length();
                    type = TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10 << 6;
                    if(identifier < 256 && identifier >= 0) {
                        type |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
                    } else {
                        type |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
                    }

                    if(len < 8 && len >= 0) {

                        type |= TypeBits43_NoTypeLengthField_00 << 3;
                        type |= len & 0b111;
                        ss.write(reinterpret_cast<char*>(&type), sizeof(type));

                    } else if(len > 0 && len < 256) {

                        type |= TypeBits43_8BitsTypeLengthField_01 << 3;
                        type |= length & 0b000;
                        ss.write(reinterpret_cast<char*>(&type), sizeof(type));

                    } else if(len > 255 && len <= 65535) {

                        type |= TypeBits43_16BitsTypeLengthField_10 << 3;
                        type |= length & 0b000;
                        ss.write(reinterpret_cast<char*>(&type), sizeof(type));
        
                    } else {
                        ///length is24 bits
                    }

                    if(identifier >=0 && identifier < 256) {
                        ss.write(reinterpret_cast<char*>(&identifier), 1);
                    } else {
                        std::uint16_t tmpid = htons(identifier);
                        ss.write(reinterpret_cast<char*>(&tmpid), 2);
                    }
                    
                    if(len >= 0 && len < 256) {
                        ss.write(reinterpret_cast<char*>(&len), 1);
                    } else {
                        std::uint16_t tmplen = htons(len);
                        ss.write(reinterpret_cast<char*>(&tmplen), 2);
                    }

                    ss.write(reinterpret_cast<char*>(riss.str().data()), len);
                    //out.assign(ss.str());

                } else if(rid["value"].is_string()) {

                    value.assign(rid["value"].get<std::string>());
                    length = value.length();

                } else if(rid["value"].is_boolean()) {

                    length = 1;

                } else if(rid["value"].is_number()) {

                    length = rid["value"].get<std::uint16_t>();

                } else {
                    length = 0;
                }

                type = TypeBits76_ResourceWithValue_11 << 6;
                if(identifier < 256 && identifier >= 0) {
                    type |= TypeBit5_LengthOfTheIdentifier8BitsLong_0 << 5;
                } else {
                    type |= TypeBit5_LengthOfTheIdentifier16BitsLong_1 << 5;
                }

                if(length < 8 && length > 0) {

                    type |= TypeBits43_NoTypeLengthField_00 << 3;
                    type |= length & 0b111;

                } else if(length > 7 && length < 256) {

                    type |= TypeBits43_8BitsTypeLengthField_01 << 3;
                    type |= length & 0b000;

                } else if(length > 255 && length <= 65535) {

                    type |= TypeBits43_16BitsTypeLengthField_10 << 3;
                    type |= length & 0b000;

                } else {
                    ///length is24 bits
                }

                ss.write(reinterpret_cast<char*>(&type), sizeof(type));
                if(length > 7 && length < 256) {

                    ss.write(reinterpret_cast<char*>(&identifier), 1);
                    ss.write(reinterpret_cast<char*>(&length), 1);

                } else if(length > 255 && length <= 65535) {

                    ss.write(reinterpret_cast<char*>(&identifier), sizeof(identifier));
                    ss.write(reinterpret_cast<char*>(&length), sizeof(length));

                }
                ss.write(reinterpret_cast<char*>(value.data()), value.length());
            }
        }
        out.assign(ss.str());
    }
        break;
    case ServerObjectID:
        /* code */
        break;
    
    case AccessControlObjectID:
        /* code */
        break;
    
    case DeviceObjectID:
        /* code */
        break;

    case ConnectivityMonitoringObjectID:
        /* code */
        break;

    case FirmwareUpdateObjectID:
        /* code */
        break;

    case LocationObjectID:
        /* code */
        break;

    case ConnectivityStatisticsObjectID:
        /* code */
        break;

    default:
        break;
    }
}






#endif /*__lwm2m_adapter_cpp__*/