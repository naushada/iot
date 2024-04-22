#ifndef __coap_adapter_cpp__
#define __coap_adapter_cpp__

#include "coap_adapter.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

CoAPAdapter::CoAPAdapter() {
    cumulativeOptionNumber = 0;
    response.clear();
    
    m_lwm2mAdapter = std::make_shared<LwM2MAdapter>();
    OptionNumber = {
        {0, "Reserve"},
        {1, "If-Match"},
        {2, "Unassigned"},
        {3, "Uri-Host"},
        {4, "ETag"},
        {5, "If-None-Match"},
        {6, "Observe"},
        {7, "Uri-Port"},
        {8, "Location-Path"},
        {9, "OSCORE"},
        {10, "Unassigned"},
        {11, "Uri-Path"},
        {12, "Content-Format"},
        {13, "Unassigned"},
        {14, "Max-Age"},
        {15, "Uri-Query"},
        {16, "Hop-Limit"},
        {17, "Accept"},
        {18, "Unassigned"},
        {19, "Q-Block1"},
        {20, "Location-Query"},
        {23, "Block2"},
        {27, "Block1"},
        {28, "Size2"},
        {31, "Q-Block2"},
        {35, "Proxy-Uri"},
        {39, "Proxy-Scheme"},
        {60, "Sizel"},
        {128, "Reserver"},
        {132, "Reserved"},
        {136, "Reserve"},
        {140, "Reserve"},
        {252, "Echo"},
        {258, "No-Response"},
        {292, "Request-Tag"},
    };

    ContentFormat = {
        {0, "text/plain;charset=utf-8"},
        {40, "application/link-format"},
        {41, "application/xml"},
        {42, "application/octet-stream"},
        {47, "application/exi"},
        {50, "application/json"},
        {60, "application/cbor"},
        {11542, "application/vnd.oma.lwm2m+tlv"},
        {11543, "application/vnd.oma.lwm2m+json"},
        {12119, "application/timeseries"},
        {12200, "application/ucbor"},
        {12201, "application/ucborz"},
        {12202, "application/sucbor"},
        {12203, "application/sucborz"},

    };

    ContentFormatByName = {
        {"text/plain;charset=utf-8", 0},
        {"application/link-format", 40},
        {"application/xml", 41},
        {"application/octet-stream", 42},
        {"application/exi", 47},
        {"application/json", 50},
        {"application/cbor", 60},
        {"application/vnd.oma.lwm2m+tlv", 11542},
        {"application/vnd.oma.lwm2m+json", 111543},
        {"application/timeseries", 12119},
        {"application/ucbor", 12200},
        {"application/ucborz", 12201},
        {"application/sucbor", 12202},
        {"application/sucborz", 12203},

    };

    ResponseCode = {
        {"2.01", "Created"},
        {"2.02", "Deleted"},
        {"2.03", "Valid"},
        {"2.04", "Changed"},
        {"2.05", "Content"},
        {"2.31", "Continue"},
        {"4.00", "Bad Request"},
        {"4.01", "Unauthorize"},
        {"4.02", "Bad Option"},
        {"4.03", "Forbidden"},
        {"4.04", "Not Found"},
        {"4.05", "Method Not Allowed"},
        {"4.06", "Not Acceptable"},
        {"4.07", "Unassigned"},
        {"4.08", "Request Entity Incomplete"},
        {"4.09", "Conflict"},
        {"4.12", "Precondition Failed"},
        {"4.13", "Request Entity Too Large"},
        {"4.15", "Unsupported Content-Format"},
        {"5.00", "Internal Server Error"},
        {"5.01", "Not Implemented"},
        {"5.02", "Bad Gateway"},
        {"5.03", "Service Unavailable"},
        {"5.04", "Gateway Timeout"},
        {"5.05", "Proxying Not Supported"},
        {"5.08", "Hop Limit Reached"}
    };

    MethodCode = {
        {1, "GET"},
        {2, "POST"},
        {3, "PUT"},
        {4, "DELETE"},
        {5, "FETCH"},
        {6, "PATCH"},
        {7, "iPATCH"}
    };

    MethodCodeByName = {
        {"GET", 1},
        {"POST", 2},
        {"PUT", 3},
        {"DELETE", 4},
        {"FETCH", 5},
        {"PATCH", 6},
        {"iPATCH", 7}
    };

    RequestType = {
        {0, "Confirmable"},
        {1, "Non-COnfirmable"},
        {2, "Acknowledgement"},
        {3, "Rest"}
    };

    CoAPUri = {"set", "get", "push", "execute"};
}

CoAPAdapter::~CoAPAdapter() {
}

bool CoAPAdapter::serialise(const std::vector<std::string> &uris, const std::vector<std::string> &queries, const std::vector<std::string>& requests,
                            const std::uint16_t& cf, const std::uint8_t& method, std::vector<std::string>& out) {
    bool ret = false;
    std::stringstream ss;
    std::uint32_t header;
    bool isBlock = false;

    if(requests.size() > 1) {
        isBlock = true;
    }

    if(requests.empty()) {
        ss.str("");
        std::uint16_t offset = 11;
        std::uint16_t msgid = rand();
        
        header = (htons(msgid)     << 16)
                |((method & 0xFF)  << 8)
                |0x44              << 0;

        ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
        std::uint32_t token = htonl(rand());
        ss.write (reinterpret_cast <const char *>(&token), sizeof(token));

        ///Options encoding
        std::uint8_t optdelta;
        optdelta = (offset & 0xF) /*Uri-Path*/ << 4;

        for(const auto& uri: uris) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " uri: " << uri << std::endl;
            if(uri.length() >= 0 && uri.length() <= 12) {
                optdelta |= (uri.length() & 0xF);
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 12 && uri.length() < 269) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = uri.length() - 13;
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(uri.length() - 269);
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            }
            optdelta = 0;
        }

        optdelta = (12 - offset /*Content-Format - Uri-Path*/) << 4;
        offset += (12 - offset);
            
        if(cf < 256) { 
            optdelta |= 1 /*Length of cf value (1 bytes)*/;
            std::uint8_t ct = cf;
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(&ct), sizeof(ct));
        }

        if(!queries.empty()) {
            optdelta = (15 - offset /* Uri-Query - Uri-Path*/) << 4;
            offset += (15 - offset);
        }
        
        for(const auto& query: queries) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " query: " << query << std::endl;
            if(query.length() > 0 && query.length() <= 12) {
                optdelta |= query.length();
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 12 && query.length() < 269) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = query.length() - 13;
                ///extended length of one byte
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(query.length() - 269);
                ///extended length of 2 bytes
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            }
            optdelta = 0;
        }

        out.push_back(ss.str());
        return(true);
    }

    size_t idx = 0;
    for(const auto& request: requests) {
        ss.str("");
        std::uint16_t offset = 11;
        std::uint16_t msgid = rand();
        
        header = (htons(msgid)             << 16)
                |(/*POST*/(method & 0xFF)  << 8)
                |0x44                      << 0;

        ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
        std::uint32_t token = htonl(rand());
        ss.write (reinterpret_cast <const char *>(&token), sizeof(token));

        ///Options encoding
        std::uint8_t optdelta;
        optdelta = (offset & 0xF) /*Uri-Path*/ << 4;
        for(const auto& uri: uris) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " uri: " << uri << std::endl;
            if(uri.length() >= 0 && uri.length() <= 12) {
                optdelta |= (uri.length() & 0xF);
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 12 && uri.length() < 269) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = uri.length() - 13;
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(uri.length() - 269);
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            }
            optdelta = 0;
        }

        if(!request.empty() && !uris.empty()) {
            optdelta = (12 - offset /*Content-Format - Uri-Path*/) << 4;
            offset += (12 - offset);
            
            if(cf < 256) { 
                optdelta |= 1 /*Length of cf value (1 bytes)*/;
                std::uint8_t ct = cf;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(&ct), sizeof(ct));
            } else {
                optdelta |= 2 /*Length of cf value (2 bytes)*/;
                std::uint16_t ct = htons(cf);
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(&ct), sizeof(ct));
            }
            
        } else if(!request.empty()) {
            offset = 12;
            optdelta = (offset /*Content-Format*/) << 4;
            optdelta |= 2 /*Length of cf value (2 bytes)*/;
            auto ct = htons(cf);
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(&ct), sizeof(cf));
        }

        if(!queries.empty()) {
            optdelta = (15 - offset /* Uri-Query - Uri-Path*/) << 4;
            offset += (15 - offset);
        }
        
        for(const auto& query: queries) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " query: " << query << std::endl;
            if(query.length() > 0 && query.length() <= 12) {
                optdelta |= query.length();
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 12 && query.length() < 269) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = query.length() - 13;
                ///extended length of one byte
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(query.length() - 269);
                ///extended length of 2 bytes
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            }
            optdelta = 0;
        }

        if(isBlock) {
            optdelta = (27 - offset) << 4;
            optdelta |= 1;
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));

            std::uint8_t blk = ((idx & 0xF) << 4) | (0xE & 0xF);
            if((idx+1) == requests.size()) {
                blk = ((idx & 0xF) << 4) | (0x6 & 0xF);
            }

            ss.write (reinterpret_cast <const char *>(&blk), sizeof(blk));
        }

        if(!request.empty()) {
            ///This is an header delimiter
            optdelta = 0xFF;
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(request.data()), request.length());
        }
        
        //ss << request;
        out.push_back(ss.str());
        ++idx;
    }

    return(true);
}
bool CoAPAdapter::serialisePOST(const std::vector<std::string> &uris, const std::vector<std::string> &queries, 
                                const std::vector<std::string>& requests, const std::uint16_t& cf, std::vector<std::string>& out) {
    bool ret = false;
    std::stringstream ss;
    std::uint32_t header;
    bool isBlock = false;

    if(requests.size() > 1) {
        isBlock = true;
    }

    size_t idx = 0;
    for(const auto& request: requests) {
        ss.str("");
        std::uint16_t offset = 11;
        std::uint16_t msgid = rand();
        
        header = (htons(msgid)         << 16)
                |(/*POST*/(2 & 0xFF)   << 8)
                |0x44                  << 0;

        ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
        std::uint32_t token = htonl(rand());
        ss.write (reinterpret_cast <const char *>(&token), sizeof(token));

        ///Options encoding
        std::uint8_t optdelta;
        optdelta = (offset & 0xF) /*Uri-Path*/ << 4;
        for(const auto& uri: uris) {
            std::cout << basename(__FILE__) << ":" << " uri: " << uri << std::endl;
            if(uri.length() >= 0 && uri.length() <= 12) {
                optdelta |= (uri.length() & 0xF);
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 12 && uri.length() <= 268) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = uri.length() - 13;
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            } else if(uri.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(uri.length() - 269);
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(uri.data()), uri.length());
            }
            optdelta = 0;
        }

        if(!request.empty() && !uris.empty()) {
            optdelta = (12 - offset /*Content-Format - Uri-Path*/) << 4;
            offset += (12 - offset);
            optdelta |= 2 /*Length of cf value (2 bytes)*/;
            auto ct = htons(cf);
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(&ct), sizeof(cf));
        } else if(!request.empty()) {
            offset = 12;
            optdelta = (offset /*Content-Format*/) << 4;
            optdelta |= 2 /*Length of cf value (2 bytes)*/;
            auto ct = htons(cf);
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(&ct), sizeof(cf));
        }

        if(!queries.empty()) {
            optdelta = (15 - offset /* Uri-Query - Uri-Path*/) << 4;
            offset += (15 - offset);
        }
        
        for(const auto& query: queries) {
            std::cout << basename(__FILE__) << ":" << " query: " << query << std::endl;
            if(query.length() > 0 && query.length() <= 12) {
                optdelta |= query.length();
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 12 && query.length() <= 268) {
                std::uint8_t optlen = 0;
                optdelta |= 13;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = query.length() - 13;
                ///extended length of one byte
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            } else if(query.length() > 268) {
                std::uint16_t optlen = 0;
                optdelta |= 14;
                ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
                optlen = htons(query.length() - 269);
                ///extended length of 2 bytes
                ss.write (reinterpret_cast <const char *>(&optlen), sizeof(optlen));
                ss.write (reinterpret_cast <const char *>(query.data()), query.length());
            }
            optdelta = 0;
        }

        if(isBlock) {
            optdelta = (27 - offset) << 4;
            optdelta |= 1;
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));

            std::uint8_t blk = ((idx & 0xF) << 4) | (0xE & 0xF);
            if((idx+1) == requests.size()) {
                blk = ((idx & 0xF) << 4) | (0x6 & 0xF);
            }

            ss.write (reinterpret_cast <const char *>(&blk), sizeof(blk));
        }

        if(!request.empty()) {
            ///This is an header delimiter
            optdelta = 0xFF;
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
        }
        
        ss.write (reinterpret_cast <const char *>(request.data()), request.length());
        //ss << request;
        out.push_back(ss.str());
        ++idx;
    }

    return(true);
}

bool CoAPAdapter::buildRequest(const std::string &plainText, std::vector<std::string>& request) {
    if(plainText.empty()) {
        /// Input buffer is empty
        return(false);
    }
    ///Convert into CBOR from plainText.
    std::string cbor;
    auto ret = getCBORAdapter().json2cbor(plainText, cbor);
    if(cbor.length() >= 1024) {
        ///zip this cbor.
        std::string compressed;
        if(compress(cbor, compressed)) {

            std::istringstream istrstr;
            std::vector<std::uint8_t> chunked(1024);
            
            istrstr.rdbuf()->pubsetbuf(const_cast<char *>(compressed.data()), compressed.length());

            while(!istrstr.read(reinterpret_cast<char *>(chunked.data()), 1024).eof()) {
                chunked.resize(istrstr.gcount());
                request.push_back(std::string(chunked.begin(), chunked.end()));
            }

            if(istrstr.gcount()) {
                chunked.resize(istrstr.gcount());
                request.push_back(std::string(chunked.begin(), chunked.end()));
            }

            return(true);
        }

    } else {
        request.push_back(cbor);
        return(true);
    }

    return(false);
}

std::string CoAPAdapter::buildRegistrationAck(const CoAPMessage& message) {
    /// This is a /rd -- registration request
    std::stringstream ss;
    std::uint32_t header;
    header = (htons(message.coapheader.msgid) << 16) 
            | (/*Code 2.04*/0x44 << 8) 
            | ((/*ver*/1         << 6) 
            | (/*ACK*/2          << 4) 
            | (message.coapheader.tokenlength & 0x0F));

    ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
    ss.write (reinterpret_cast <const char *>(message.tokens.data()), message.tokens.size());

#if 0
    /// Encode URI also.
    for(const auto& ent: message.uripath) {
        std::uint8_t onebyte;
//        std::cout << basename(__FILE__) << ":" << __LINE__ << " delta:" << OptionNumber[ent.optiondelta] << " length:" << ent.optionlength << " value:" << ent.optionvalue << std::endl;
        if(!OptionNumber[ent.optiondelta].compare(0, 8, "Uri-Path")) {
            onebyte = ent.optiondelta << 4;
            if(ent.optionlength > 0 && ent.optionlength < 13) {
                onebyte |= ent.optionlength;
                ss.write (reinterpret_cast <const char *>(&onebyte), sizeof(onebyte));
                ss.write (reinterpret_cast <const char *>(ent.optionvalue.data()), ent.optionvalue.size());
            } else if(ent.optionlength >= 13 && ent.optionlength < 269) {
                //extended length 
                onebyte |= 13;
                std::uint8_t extendedlength = ent.optionvalue.length() - 13;
                ss.write (reinterpret_cast <const char *>(&onebyte), sizeof(onebyte));
                ss.write (reinterpret_cast <const char *>(&extendedlength), sizeof(extendedlength));
                ss.write (reinterpret_cast <const char *>(ent.optionvalue.data()), ent.optionvalue.size());
            } else {
                //extended length 
                onebyte |= 14;
                std::uint16_t extendedlength = ent.optionvalue.length() - 14;
                ss.write (reinterpret_cast <const char *>(&onebyte), sizeof(onebyte));
                ss.write (reinterpret_cast <const char *>(&extendedlength), sizeof(extendedlength));
                ss.write (reinterpret_cast <const char *>(ent.optionvalue.data()), ent.optionvalue.size());
            }
        }
    }
#endif
    return(ss.str());
}

std::string CoAPAdapter::buildPushContinue(const CoAPMessage& message) {
    /// This is a /rd -- registration request
    std::stringstream ss;
    std::uint32_t header;
    header = (htons(message.coapheader.msgid) << 16) 
            | (/*Code 2.31*/0x5F << 8)
            | ((/*ver*/1         << 6)
            | (/*ACK*/2          << 4)
            | (message.coapheader.tokenlength & 0x0F));

    ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
    ss.write (reinterpret_cast <const char *>(message.tokens.data()), message.tokens.size());

    auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool 
                    {return(getOptionNumber(ent.optiondelta).compare("Block1") == 0);});
                
    if(it != message.uripath.end()) {
        std::uint8_t option = (13 << 4) /*option delta extended*/ | 1 /*option length*/;
        ss.write (reinterpret_cast <const char *>(&option), sizeof(option));
        /* option delta extended value */
        option = 0x0E;/*27({27, "Block1"},) - 13 (optiondelta) = 14*/
        ss.write (reinterpret_cast <const char *>(&option), sizeof(option));
        /* Option length value*/
        auto ent = *it;
        option = ent.optionvalue.at(0);
        ss.write (reinterpret_cast <const char *>(&option), sizeof(option));
    }

    return(ss.str());
}

std::string CoAPAdapter::buildPushAck(const CoAPMessage& message) {
    std::stringstream ss;
    std::uint32_t header;
    header = (htons(message.coapheader.msgid) << 16) 
            | (/*Code 2.01*/0x41 << 8) 
            | ((/*ver*/1         << 6) 
            | (/*ACK*/2          << 4) 
            | (message.coapheader.tokenlength & 0x0F));

    ss.write (reinterpret_cast <const char *>(&header), sizeof(header));
    ss.write (reinterpret_cast <const char *>(message.tokens.data()), message.tokens.size());
    return(ss.str());
}

std::string CoAPAdapter::buildResponse(const CoAPMessage& message) {
    ///Is this a registration request?
    auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {
        return((ent.optionvalue == "rd") ||
        (ent.optionvalue == "bs"));
    });

    if(it != message.uripath.end()) {
        /// This is a /rd or /bs-- registration request
        return(buildRegistrationAck(message));
    }

    it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {return(ent.optionvalue == "push");});
    if(it != message.uripath.end()) {
        /// This is a /push -- CoAP Push request
        if(message.ismorebitset) {
            return(buildPushContinue(message));
        } else {
           return(buildPushAck(message));
        }
    }

    if(RequestType[message.coapheader.type].compare("Acknowledgement")) {
        /// Type is not an ACK, we got a request
        //return(buildPushAck(message));
    }

    std::string uri;
    if(!isCoAPUri(message, uri)) {
        ///Build ACK
    }

    ///Is this a LwM2M Objects
    std::uint32_t oid = 0, oiid = 0, rid = 0, riid = 0;
    if(isLwm2mUriObject(message, oid, oiid, rid, riid)) {
        LwM2MAdapter lwm2mAdapter;
        LwM2MObjectData data;
        LwM2MObject object;
        data.m_oiid = oiid;
        data.m_rid = rid;
        data.m_riid = riid;
        object.m_oid = oid;
        lwm2mAdapter.parseLwM2MObjects(message.payload, data, object);
        {
            ///Objects are extracted successfully
            for(const auto& ent: object.m_value) {
                std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                          << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                          << " ent.m_ridvalue:";
        
                for(const auto& elm: ent.m_ridvalue) {
                    printf("%0.2X ", (std::uint8_t)elm);
                }
                printf("\n");
            }

            if(RequestType[message.coapheader.type].compare("Acknowledgement")) {
                /// Type is not an ACK, we got a request
                return(buildPushAck(message));
            }           
        }
    } else {
        std::cout <<basename(__FILE__) << ":" << __FILE__ << " This is not an LwM2M Object" << std::endl;
    }
    return(std::string());
}

std::int32_t CoAPAdapter::parseRequest(const std::string& in, CoAPMessage& coapmessage) {

    std::istringstream istrstr;
    istrstr.rdbuf()->pubsetbuf(const_cast<char *>(in.data()), in.length());
    std::uint8_t onebyte;
    std::uint16_t mid;
    std::uint32_t fourbytes;
    resetCummulativeOptionNumber();

    do {
        if(!istrstr.read(reinterpret_cast<char *>(&fourbytes), sizeof(fourbytes)).good()) {
            std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
            break;    
        }

        coapmessage.coapheader.tokenlength = fourbytes & 0b00001111;
        coapmessage.coapheader.type = (fourbytes & 0b00110000) >> 4;
        coapmessage.coapheader.ver = (fourbytes & 0b11000000) >> 6;
        coapmessage.coapheader.code = (fourbytes >> 8) & 0b11111111;
        coapmessage.coapheader.msgid = ntohs((fourbytes >> 16) & 0xFFFF);

        if(coapmessage.coapheader.tokenlength > 0) {
            size_t len = static_cast<size_t>(coapmessage.coapheader.tokenlength);
            coapmessage.tokens.resize(len);
            if(!istrstr.read(reinterpret_cast<char *>(coapmessage.tokens.data()), len).good()) {
                std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                break;
            }
        }

        /// Options field
        CoAPOptions opt;
        while(istrstr.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
            opt.optiondelta = (onebyte & 0b11110000) >> 4;
            opt.optionlength = onebyte & 0b00001111;

            if(opt.optiondelta == 13) {

                if(!istrstr.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }
                opt.optiondelta = onebyte + 13;

            } else if(opt.optiondelta == 14) {

                if(!istrstr.read(reinterpret_cast<char *>(&mid), sizeof(mid)).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }

                opt.optiondelta = ntohs(mid) + 269;

            } else if (opt.optiondelta == 15 && opt.optionlength == 15) {
                ///do we have chunked/blocked payload?
                auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
                    return(getOptionNumber(ent.optiondelta).compare("Block1") == 0);
                });

                coapmessage.ismorebitset = false;
                std::vector<std::uint8_t> contents(1024);
                if(it != coapmessage.uripath.end()) {
                    auto ent = *it;
                    auto blk1 = static_cast<std::uint8_t>(ent.optionvalue.at(0));
                    auto szx = blk1 & 0b111;
                    auto m = (blk1 >> 3) & 0b1;
                    auto num = (blk1 >> 4) & 0b1111;

                    auto len =  1 << (szx + 4);

                    coapmessage.ismorebitset = (m == 1) ? true : false;
                    contents.resize(len);
                    if(!istrstr.read(reinterpret_cast<char *>(contents.data()), len).good()) {
                        /// The last block might be less than 1024 bytes while len says it's 1024.
                        if(istrstr.eof()) {
                            std::cout << basename(__FILE__) << ":" << __LINE__ << " expected length: " << len << std::endl;
                            auto len = istrstr.gcount();
                            std::cout << basename(__FILE__) << ":" << __LINE__ << " actual length: " << len << std::endl;
                            contents.resize(len);
                        }
                    }
                    
                } else {
                    
                    if(istrstr.read(reinterpret_cast<char *>(contents.data()), contents.size()).eof()) {
                        /// The last block might be less than 1024 bytes while len says it's 1024.
                        auto len = istrstr.gcount();
                        std::cout << basename(__FILE__) << ":" << __LINE__ << " actual length: " << len << std::endl;
                        contents.resize(len);
                    }
                }
                coapmessage.payload += std::string(contents.begin(), contents.end());
                break;
            }

            if(opt.optionlength >=0 && opt.optionlength <= 12) {

                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }

            } else if(opt.optionlength == 13) {

                if(!istrstr.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }

                opt.optionlength = onebyte + 13;
                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }

            } else if(opt.optionlength == 14) {

                if(!istrstr.read(reinterpret_cast<char *>(&mid), sizeof(mid)).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }

                opt.optionlength = ntohs(mid) + 269;
                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    std::cout <<basename(__FILE__) << ":" << __LINE__ << " input buffer is too small to process" << std::endl;
                    break;
                }
            }

            auto whatis = SumOptionNumber(static_cast<std::uint32_t>(opt.optiondelta));
            opt.optiondelta = whatis;
            coapmessage.uripath.push_back(opt);
        }
    }while(0);

    dumpCoAPMessage(coapmessage);
    return(0);
}

bool CoAPAdapter::uncompress(const std::string &input, std::string &output) {
    if (input.empty()) {
        return false;
    }

    std::int32_t inflateChunk = 16 * 1024;
    z_stream zStream;
    zStream.zalloc = Z_NULL;
    zStream.zfree = Z_NULL;
    zStream.opaque = Z_NULL;

    if(inflateInit(&zStream) != Z_OK) {
        std::cout<< basename(__FILE__) << ":" << __LINE__ << " could not initialize uncompression" << std::endl;
        inflateEnd(&zStream);
        return false;
    }

    output.clear();
    zStream.avail_in = static_cast<uInt>(input.size());
    zStream.next_in = (Bytef*) &input[0];
    char out[inflateChunk];
    int inflateStatus;

    do {
        zStream.avail_out = sizeof(out);
        zStream.next_out = (Bytef*) out;
        inflateStatus = inflate(&zStream, Z_FINISH);
        if(inflateStatus == Z_STREAM_ERROR
                || inflateStatus == Z_NEED_DICT
                || inflateStatus == Z_DATA_ERROR
                || inflateStatus == Z_MEM_ERROR) {
            std::cout << basename(__FILE__) << ":" << __LINE__  << " could not uncompress" << std::endl;
            inflateEnd(&zStream);
            output.clear();
            return false;
        }

        output.append(out, sizeof(out) - zStream.avail_out);
    }

    while(zStream.avail_out == 0);
    if(inflateEnd(&zStream) != Z_OK) {
        std::cout<<basename(__FILE__) << ":" << __LINE__  << " could not finalize uncompression" << std::endl;
        output.clear();
        return false;
    }

    if (!output.empty()) {
        std::cout << basename(__FILE__) << ":" << __LINE__  << " uncompression ratio input/output: " 
                  << double(input.size())/output.size() << std::endl;
    } else {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " uncompression output empty" << std::endl;
    }
    return true;
}

bool CoAPAdapter::compress(const std::string &input, std::string &output) {
    if (input.empty()) {
        return false;
    }

    z_stream zStream;
    zStream.zalloc = Z_NULL;
    zStream.zfree = Z_NULL;
    zStream.opaque = Z_NULL;
    if (deflateInit(&zStream, Z_BEST_COMPRESSION) != Z_OK) {
        deflateEnd(&zStream);
        return false;
    }

    zStream.avail_in = static_cast<uInt>(input.size());
    zStream.next_in = (Bytef*) &input[0];
    // allocate output to expected size
    output.resize(deflateBound(&zStream, zStream.avail_in));
    zStream.avail_out = static_cast<uInt>(output.size());
    zStream.next_out = (Bytef*) &output[0];

    if (deflate(&zStream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zStream);
        output.clear();
        return false;
    }

    if (deflateEnd(&zStream) != Z_OK) {
        output.clear();
        return false;
    }

    output.resize(zStream.total_out);
    if (!output.empty()) {
        std::cout << basename(__FILE__) << ":" << __LINE__  << " compression ratio input/output: "
            << double(input.size())/output.size() << ", output size: " << output.size() << std::endl;
    } else {
        std::cout << basename(__FILE__) << ":" << __LINE__  << " compression output empty" << std::endl;
    }
    return true;
}

bool CoAPAdapter::writeIntoFile(const std::string &input, const std::string& fileName) {

    std::ofstream ofs;
    ofs.open(fileName);

    if(!ofs.is_open()) {
        std::cout << basename(__FILE__) << ":" << " Opening of file: " << fileName << " is Failed" << std::endl;
        return(false);
    }

    ofs << input;
    ofs.close();
    return(true);
}

std::vector<std::string> CoAPAdapter::handleLwM2MObjects(const CoAPAdapter::CoAPMessage& message, std::string uri, std::uint32_t oid, std::uint32_t oiid, std::uint32_t rid, std::uint32_t riid) {
    //std::cout << basename(__FILE__) << ":" << __LINE__ << " value of uri: "  << uri << std::endl;
    std::vector<std::string> out;
    std::string rsp;
    /// This is LwM2M string URI rd or bs
    if(uri == "bs") {
        rsp = buildResponse(message);
        out.push_back(rsp);
        std::string secobj;
        std::vector<std::string> serobj;
        lwm2mAdapter()->bootstrapSecurityObject00(secobj);
        serialise({{"0"},{"0"}},{}, {{secobj}}, getContentFormat("application/vnd.oma.lwm2m+tlv"), getMethodCode("PUT"), serobj);
        out.push_back(serobj[0]);
        secobj.clear();
        serobj.clear();
        lwm2mAdapter()->devicemgmtSecurityObject01(secobj);
        serialise({{"0"},{"1"}},{}, {{secobj}}, getContentFormat("application/vnd.oma.lwm2m+tlv"), getMethodCode("PUT"), serobj);
        out.push_back(serobj[0]);
        serobj.clear();
        serialise({{"bs"}},{}, {{}}, getContentFormat("text/plain;charset=utf-8"), getMethodCode("POST"), serobj);
        out.push_back(serobj[0]);

    } else if(uri == "rd") {
        ///rd - registration update
    } else {
        LwM2MObjectData data;
        LwM2MObject object;
        data.m_oiid = oiid;
        data.m_rid = rid;
        data.m_riid = riid;
        object.m_oid = oid;

        if(!lwm2mAdapter()->parseLwM2MObjects(message.payload, data, object)) {
            for(const auto& ent: object.m_value) {
                std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                          << " ent.m_rid:" << lwm2mAdapter()->resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                          << " ent.m_ridvalue:";
                
                for(const auto& elm: ent.m_ridvalue) {
                    printf("%0.2X ", (std::uint8_t)elm);
                }
                printf("\n");
            }
            rsp = buildResponse(message);
            out.push_back(rsp);
        }
    }
    return(out);
}

std::int32_t CoAPAdapter::processRequest(const std::string& in, std::vector<std::string>& out) {
    cumulativeOptionNumber = 0;
    CoAPMessage coapmessage;
    std::string rsp;
    auto ret = parseRequest(in, coapmessage);

    auto cf = getContentFormat(coapmessage);
    //std::cout << basename(__FILE__) << ":" << __LINE__ << " value of cf: "  << cf << std::endl;
    if(cf.length() > 0 && (cf == "application/vnd.oma.lwm2m+tlv") || (cf == "text/plain;charset=utf-8")) {
        ///Build the Response for a given Request
        std::string uri;
        std::uint32_t oid = 0, oiid = 0, rid = 0, riid = 0;
        if(isLwm2mUri(coapmessage, uri, oid, oiid, rid, riid)) {
            out = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
        }
    } else if(!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        rsp = buildResponse(coapmessage);
        out.push_back(rsp);
    } else {
        /// This is a CoAP Request
        rsp = buildResponse(coapmessage);
        out.push_back(rsp);
    }

    if(!coapmessage.ismorebitset) {
        
        ///check for content type
        auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
            return(getOptionNumber(ent.optiondelta).compare("Content-Format") == 0);
        });
        
        if(it != coapmessage.uripath.end()) {
            auto ent = *it;
            std::uint16_t ct = ntohs(*reinterpret_cast<std::uint16_t *>(ent.optionvalue.data()));
            std::string output;

            if(12201/*UCBORZ*/ == ct) {
                ///decompress the contents.
                if(uncompress(coapmessage.payload, output)) {
                    ///uncompressed successfully.
                    writeIntoFile(output, "ucborz_cf_12201.txt");
                    auto payload = json::from_cbor(output.c_str());
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " The Response is\n" << payload.dump() << std::endl;
                }
            } else if(12119/*TS*/ == ct) {
                writeIntoFile(coapmessage.payload, "ts_cf_12119.txt");
            } else if(12200/*UCBOR*/ == ct) {
                writeIntoFile(coapmessage.payload, "ucbor_cf_12200.txt");
                auto payload = json::from_cbor(coapmessage.payload.c_str());
                for(auto&[key, value]: payload.items()) {
                    
                }
                std::cout << basename(__FILE__) << ":" << __LINE__ << " The Response is\n" << payload.dump() << std::endl;

            } else if(12202/*SUCBOR*/ == ct) {
                writeIntoFile(coapmessage.payload, "sucbor_cf_12202.txt");
            } else if(12203 /*SUCBORZ*/ == ct) {
                /// @brief this is a signed payload
                if(uncompress(coapmessage.payload, output)) {
                    ///uncompressed successfully.
                    writeIntoFile(output, "sucbor_cf_12203.txt");
                }
            } else if(11542 /*application/vnd.oma.lwm2m+tlv*/ == ct) {
                ///Build the Response for a given Request
                std::string uri;
                std::uint32_t oid, oiid, rid, riid;
                if(isLwm2mUriObject(coapmessage, oid, oiid, rid, riid)) {
                    /// This is LwM2M Object URI, Handle it.
                    LwM2MObjectData data;
                    LwM2MObject object;
                    data.m_oiid = oiid;
                    data.m_rid = rid;
                    data.m_riid = riid;
                    object.m_oid = oid;

                    if(!lwm2mAdapter()->parseLwM2MObjects(coapmessage.payload, data, object)) {

                        ///Objects are extracted successfully
                        for(const auto& ent: object.m_value) {
                            std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                                      << " ent.m_rid:" << lwm2mAdapter()->resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                                      << " ent.m_ridvalue:";
        
                            for(const auto& elm: ent.m_ridvalue) {
                                printf("%0.2X ", (std::uint8_t)elm);
                            }
                            printf("\n");
                        }
                    }
                }
            }
        }        
    }

    return(out.size());
}

std::int32_t CoAPAdapter::processRequest(session_t* session, std::string& in, std::vector<std::string>& out) {
    /// clear the response buffer now.
    cumulativeOptionNumber = 0;
    CoAPMessage coapmessage;
    auto ret = parseRequest(in, coapmessage);
    auto cf = getContentFormat(coapmessage);
    std::cout << basename(__FILE__) << ":" << __LINE__ << " processRequest with session" << std::endl;

    if(cf.length() > 0 && (cf == "application/vnd.oma.lwm2m+tlv") || (cf == "text/plain;charset=utf-8")) {

        ///Build the Response for a given Request
        std::string uri;
        std::uint32_t oid, oiid, rid, riid;
        if(isLwm2mUri(coapmessage, uri, oid, oiid, rid, riid)) {
            /// This is LwM2M string URI rd or bs
            out = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
            return(out.size());
        }

    } else if(!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        ///response is a calls variable.
        response = buildResponse(coapmessage);
    } else {

        ///This is a CoAP Request
        dumpCoAPMessage(coapmessage);
        auto rsp = buildResponse(coapmessage);
        out.push_back(rsp);
    }

    if(!coapmessage.ismorebitset) {
        
        ///check for content type
        auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
            return(getOptionNumber(ent.optiondelta).compare("Content-Format") == 0);
        });
        
        if(it != coapmessage.uripath.end()) {
            auto ent = *it;
            std::uint16_t ct = ntohs(*reinterpret_cast<std::uint16_t *>(ent.optionvalue.data()));
            std::string output;

            if(12201/*UCBORZ*/ == ct) {
                ///decompress the contents.
                if(uncompress(coapmessage.payload, output)) {
                    ///uncompressed successfully.
                    writeIntoFile(output, "ucborz_cf_12201.txt");
                }
            } else if(12119/*TS*/ == ct) {
                writeIntoFile(coapmessage.payload, "ts_cf_12119.txt");
            } else if(12200/*UCBOR*/ == ct) {
                writeIntoFile(coapmessage.payload, "ucbor_cf_12200.txt");
            } else if(12202/*SUCBOR*/ == ct) {
                writeIntoFile(coapmessage.payload, "sucbor_cf_12202.txt");
            } else if(12203 /*SUCBORZ*/ == ct) {
                if(uncompress(coapmessage.payload, output)) {
                    ///uncompressed successfully.
                    writeIntoFile(output, "sucbor_cf_12203.txt");
                }
            } else if(11542 /*application/vnd.oma.lwm2m+tlv*/ == ct) {
                ///Process LwM2M Object(s)
                LwM2MAdapter lwm2mAdapter;
                ///Build the Response for a given Request
                std::string uri;
                std::uint32_t oid, oiid, rid, riid;
                if(isLwm2mUri(coapmessage, uri, oid, oiid, rid, riid)) {
                    /// This is aLwM2M string URI rd or bs
                } else if(isLwm2mUriObject(coapmessage, oid, oiid, rid, riid)) {
                    /// This is LwM2M Object URI, Handle it.
                    LwM2MObjectData data;
                    LwM2MObject object;
                    data.m_oiid = oiid;
                    data.m_rid = rid;
                    data.m_riid = riid;
                    object.m_oid = oid;

                    if(!lwm2mAdapter.parseLwM2MObjects(coapmessage.payload, data, object)) {

                        ///Objects are extracted successfully
                        for(const auto& ent: object.m_value) {
                            std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                                      << " ent.m_rid:" << lwm2mAdapter.resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                                      << " ent.m_ridvalue:";
        
                            for(const auto& elm: ent.m_ridvalue) {
                                printf("%0.2X ", (std::uint8_t)elm);
                            }
                            printf("\n");
                        }
                    }
                }
            }
        }        
    }

    return(response.length());
}

void CoAPAdapter::dumpCoAPMessage(const CoAPMessage& coapmessage) {
    
    std::stringstream ss;
    if(getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type)) == "Acknowledgement") {
        ss << ((coapmessage.coapheader.code >> 5) & 0b111) << "." << std::setw(2) << std::setfill('0') << (coapmessage.coapheader.code & 0b11111);
        auto str = ResponseCode[ss.str()];
        ss <<" " << str;
    } else {
        ss << getMethodCode(static_cast<std::uint32_t>(coapmessage.coapheader.code));
    }
    std::cout << std::endl << basename(__FILE__) << ":" << __LINE__ 
              << " ver: "         << std::to_string(coapmessage.coapheader.ver)
              << " type: "        << getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type))
              << " tokenlength: " << std::to_string(coapmessage.coapheader.tokenlength)
              //<< " code: "        << getMethodCode(static_cast<std::uint32_t>(coapmessage.coapheader.code))
              << " code: "        << ss.str()
              << " msgid: "       << coapmessage.coapheader.msgid << std::endl;

    for(auto const& opt: coapmessage.uripath) {
        std::cout << " optiondelta: "  << getOptionNumber(static_cast<std::uint32_t>(opt.optiondelta)) 
                  << " optionlength: " << std::to_string(opt.optionlength)
                  << " optionvalue: "  << opt.optionvalue
                  << std::endl;
    }

    auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
        return(getOptionNumber(ent.optiondelta).compare("Block1") == 0);
    });

    if(it != coapmessage.uripath.end()) {
        auto ent = *it;
        auto blk1 = static_cast<std::uint8_t>(ent.optionvalue.at(0));
        printf("Block1 Length: %0.2X\n", blk1);
        auto szx = blk1 & 0b111;
        auto m = (blk1 >> 3) & 0b1;
        auto num = (blk1 >> 4) & 0b1111;

        std::cout << basename(__FILE__) << ":" << __LINE__ << " Block Number: " << std::to_string(num)
                  << " More: " << std::to_string(m)
                  << " Block Size: " << std::to_string(szx)
                  << std::endl;
    }
}









#endif /*__coap_adapter_cpp__*/