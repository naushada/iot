#ifndef __coap_adapter_cpp__
#define __coap_adapter_cpp__

#include "coap_adapter.hpp"
#include "lwm2m_bootstrap_client.hpp"
#include "lwm2m_bootstrap_server.hpp"
#include "lwm2m_dm_client.hpp"
#include "lwm2m_registration_client.hpp"
#include "lwm2m_registration_server.hpp"
#include "nlohmann/json.hpp"
#include <ace/Log_Msg.h>

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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l uri: %C\n"),
                       uri.c_str()));
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
        } else {
            optdelta |= 2 /*Length of cf value (2 bytes)*/;
            std::uint16_t ct = htons(cf);
            ss.write (reinterpret_cast <const char *>(&optdelta), sizeof(optdelta));
            ss.write (reinterpret_cast <const char *>(&ct), sizeof(ct));
        }

        if(!queries.empty()) {
            optdelta = (15 - offset /* Uri-Query - Uri-Path*/) << 4;
            offset += (15 - offset);
        }

        for(const auto& query: queries) {
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l query: %C\n"),
                       query.c_str()));
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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l uri: %C\n"),
                       uri.c_str()));
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
                optlen = ::htons(uri.length() - 269);
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
                std::uint16_t ct = ::htons(cf);
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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l query: %C\n"),
                       query.c_str()));
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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l uri: %C\n"),
                       uri.c_str()));
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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l query: %C\n"),
                       query.c_str()));
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
        if(RequestType[message.coapheader.type].compare("Acknowledgement")) {
            /// Type is not an ACK, we got a request
            return(buildPushAck(message));
        }

        #if 0
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
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l object.m_oid:%u "
                                    "ent.m_oiid:%u ent.m_riid:%u ent.m_rid:%C "
                                    "ent.m_ridlength:%u ent.m_ridvalue.size:%u\n"),
                           static_cast<unsigned>(object.m_oid),
                           static_cast<unsigned>(ent.m_oiid),
                           static_cast<unsigned>(ent.m_riid),
                           lwm2mAdapter.resourceIDName(oid, ent.m_rid).c_str(),
                           static_cast<unsigned>(ent.m_ridlength),
                           static_cast<unsigned>(ent.m_ridvalue.size())));
            }

            if(RequestType[message.coapheader.type].compare("Acknowledgement")) {
                /// Type is not an ACK, we got a request
                return(buildPushAck(message));
            }           
        }
        #endif

    } else {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l This is not an LwM2M Object\n")));
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
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
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
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
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
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
                    break;
                }
                opt.optiondelta = onebyte + 13;

            } else if(opt.optiondelta == 14) {

                if(!istrstr.read(reinterpret_cast<char *>(&mid), sizeof(mid)).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
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
                            ACE_ERROR((LM_ERROR,
                                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l expected length: %d\n"),
                                       static_cast<int>(len)));
                            auto len = istrstr.gcount();
                            ACE_DEBUG((LM_DEBUG,
                                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l actual length: %d\n"),
                                       static_cast<int>(len)));
                            contents.resize(len);
                        }
                    }

                } else {

                    if(istrstr.read(reinterpret_cast<char *>(contents.data()), contents.size()).eof()) {
                        /// The last block might be less than 1024 bytes while len says it's 1024.
                        auto len = istrstr.gcount();
                        ACE_DEBUG((LM_DEBUG,
                                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l actual length: %d\n"),
                                   static_cast<int>(len)));
                        contents.resize(len);
                    }
                }
                coapmessage.payload += std::string(contents.begin(), contents.end());
                break;
            }

            if(opt.optionlength >=0 && opt.optionlength <= 12) {

                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
                    break;
                }

            } else if(opt.optionlength == 13) {

                if(!istrstr.read(reinterpret_cast<char *>(&onebyte), sizeof(onebyte)).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
                    break;
                }

                opt.optionlength = onebyte + 13;
                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
                    break;
                }

            } else if(opt.optionlength == 14) {

                if(!istrstr.read(reinterpret_cast<char *>(&mid), sizeof(mid)).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
                    break;
                }

                opt.optionlength = ntohs(mid) + 269;
                opt.optionvalue.resize(opt.optionlength);
                if(!istrstr.read(reinterpret_cast<char *>(opt.optionvalue.data()), opt.optionlength).good()) {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l input buffer is too small to process\n")));
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
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Failed to initialize uncompression\n")));
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
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Failed to uncompress\n")));
            inflateEnd(&zStream);
            output.clear();
            return false;
        }

        output.append(out, sizeof(out) - zStream.avail_out);
    }

    while(zStream.avail_out == 0);
    if(inflateEnd(&zStream) != Z_OK) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Failed to finalize uncompression\n")));
        output.clear();
        return false;
    }

    if (!output.empty()) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l uncompression ratio input/output: %C\n"),
                   std::to_string(double(input.size())/output.size()).c_str()));
    } else {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l uncompression output empty\n")));
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
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l compression ratio input/output: %C, output size: %d\n"),
                   std::to_string(double(input.size())/output.size()).c_str(),
                   static_cast<int>(output.size())));
    } else {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l compression output empty\n")));
    }
    return true;
}

bool CoAPAdapter::writeIntoFile(const std::string &input, const std::string& fileName) {

    std::ofstream ofs;
    ofs.open(fileName);

    if(!ofs.is_open()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Opening of file: %C is Failed\n"),
                   fileName.c_str()));
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

        auto ret = lwm2mAdapter()->parseLwM2MObjects(message.payload, data, object);

        if(!ret) {
            for(const auto& ent: object.m_value) {
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l object.m_oid:%u ent.m_oiid:%u ent.m_riid:%u ent.m_rid:%C ent.m_ridlength:%u ent.m_ridvalue.size:%d ent.m_ridvalue:\n"),
                           static_cast<unsigned>(object.m_oid),
                           static_cast<unsigned>(ent.m_oiid),
                           static_cast<unsigned>(ent.m_riid),
                           lwm2mAdapter()->resourceIDName(oid, ent.m_rid).c_str(),
                           static_cast<unsigned>(ent.m_ridlength),
                           static_cast<int>(ent.m_ridvalue.size())));

                if(oid == 0 && ent.m_rid == 0) {
                    ACE_DEBUG((LM_DEBUG,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l %C\n"),
                               std::string(ent.m_ridvalue.begin(), ent.m_ridvalue.end()).c_str()));
                } else {
                    std::string hex; char b[4];
                    for(const auto& elm: ent.m_ridvalue) {
                        snprintf(b, sizeof(b), "%02X ", (std::uint8_t)elm);
                        hex += b;
                    }
                    ACE_DEBUG((LM_DEBUG,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l ridvalue(hex): %C\n"),
                               hex.c_str()));
                }
            }

            rsp = buildResponse(message);
            out.push_back(rsp);
        }
    }
    return(out);
}

std::int32_t CoAPAdapter::processRequest(bool isAmIClient, const std::string& in, std::vector<std::string>& out) {
    cumulativeOptionNumber = 0;
    CoAPMessage coapmessage;
    std::string rsp;
    auto ret = parseRequest(in, coapmessage);

    // L9 / FUP-2 — Acknowledgement-typed frames are responses to one of
    // *our* outbound CON requests (Register, Update, Deregister, …).
    // Forward them to the RegistrationClient FSM if one is attached so
    // its state advances past AwaitingRegisterAck (FUP-2); regardless,
    // we never ship a reply back at the wire layer — that would echo
    // the ACK back to whoever sent it and trigger an exception trace
    // in Leshan.
    if (!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        if (isAmIClient && m_regClient) {
            m_regClient->on_response(coapmessage, *this);
        }
        return 0;
    }

    // L4 hot path: Bootstrap dispatch (no DTLS session variant).
    if (!isAmIClient && m_bsServer) {
        auto bsr = m_bsServer->handle(coapmessage, *this);
        if (bsr.handled) {
            for (auto& f : bsr.frames) out.push_back(std::move(f));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L3 hot path (no DTLS session variant). Peer address is unknown
    // here, so the registry records empty peerHost/peerPort; the
    // UDP-only path is mostly used by tests and the plain-CoAP push
    // plane today.
    if (!isAmIClient && m_regServer) {
        auto outcome = m_regServer->handle(coapmessage, *this, std::string{}, 0);
        if (outcome.kind != ::lwm2m::RegistrationOutcome::None) {
            if (!outcome.response.empty()) out.push_back(std::move(outcome.response));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L5 hot path: client-side DM dispatcher (no DTLS variant).
    if (isAmIClient && m_dmClient) {
        auto dmOut = m_dmClient->handle(coapmessage, *this);
        if (dmOut.kind != ::lwm2m::DmOutcome::None) {
            if (!dmOut.response.empty()) out.push_back(std::move(dmOut.response));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L9 hot path: client-side Bootstrap dispatcher (no DTLS variant).
    if (isAmIClient && m_bsClient) {
        auto rsp = m_bsClient->handle_bs_traffic(coapmessage, *this);
        if (!rsp.empty()) {
            out.push_back(std::move(rsp));
            return static_cast<std::int32_t>(out.size());
        }
    }

    auto cf = getContentFormat(coapmessage);
    //std::cout << basename(__FILE__) << ":" << __LINE__ << " value of cf: "  << cf << std::endl;
    if(cf.length() > 0 && (cf == "application/vnd.oma.lwm2m+tlv") || (cf == "text/plain;charset=utf-8")) {
        ///Build the Response for a given Request
        std::string uri;
        std::uint32_t oid = 0, oiid = 0, rid = 0, riid = 0;
        if(isLwm2mUri(coapmessage, uri, oid, oiid, rid, riid)) {
            if("rd" == uri || "bs" == uri) {
                //rsp = buildResponse(coapmessage);
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l building an ACK\n")));
                
                if(!isAmIClient && !uri.compare(0, 2, "bs")) {
                    rsp = buildRegistrationAck(coapmessage);
                    out.push_back(rsp);
                    auto responses = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
                    for(auto& response: responses) {
                        out.push_back(response);
                    }
                } else if(!uri.compare(0, 2, "rd")) {
                    rsp = buildRegistrationAck(coapmessage);
                    out.push_back(rsp);
                } else {
                    rsp = buildRegistrationAck(coapmessage);
                    out.push_back(rsp);
                }
            } else {
                /// Parsing LwM2M Objects
                out = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
            }
        } else {

        }
    } else if(!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        // L9 fix: an ACK is a response; we MUST NOT reflexively send
        // another ACK back. The previous behavior pushed a 2.04 Changed
        // out which Leshan logged as "received response to a request
        // we didn't send" (frame 3 in nfr-001-coap.pcap before the fix).
        // The FSM-level handling (RegistrationClient::on_response /
        // DmClient response paths) is the proper consumer of an ACK;
        // wire that in via the L9 follow-up. For now, just stop the
        // spurious echo.
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
                    ACE_DEBUG((LM_DEBUG,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l The Response is\n%C\n"),
                               payload.dump().c_str()));
                }
            } else if(12119/*TS*/ == ct) {
                writeIntoFile(coapmessage.payload, "ts_cf_12119.txt");
            } else if(12200/*UCBOR*/ == ct) {
                writeIntoFile(coapmessage.payload, "ucbor_cf_12200.txt");
                auto payload = json::from_cbor(coapmessage.payload.c_str());
                for(auto&[key, value]: payload.items()) {
                    
                }
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l The Response is\n%C\n"),
                           payload.dump().c_str()));

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
                            ACE_DEBUG((LM_DEBUG,
                                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l object.m_oid:%u ent.m_oiid:%u ent.m_riid:%u ent.m_rid:%C ent.m_ridlength:%u ent.m_ridvalue.size:%d ent.m_ridvalue:\n"),
                                       static_cast<unsigned>(object.m_oid),
                                       static_cast<unsigned>(ent.m_oiid),
                                       static_cast<unsigned>(ent.m_riid),
                                       lwm2mAdapter()->resourceIDName(oid, ent.m_rid).c_str(),
                                       static_cast<unsigned>(ent.m_ridlength),
                                       static_cast<int>(ent.m_ridvalue.size())));
        
                            {
                                std::string hex; char b[4];
                                for(const auto& elm: ent.m_ridvalue) {
                                    snprintf(b, sizeof(b), "%02X ", (std::uint8_t)elm);
                                    hex += b;
                                }
                                ACE_DEBUG((LM_DEBUG,
                                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l ridvalue(hex): %C\n"),
                                           hex.c_str()));
                            }
                        }
                    }
                }
            }
        }        
    }

    return(out.size());
}

std::int32_t CoAPAdapter::processRequest(bool isAmIClient, session_t* session, std::string& in, std::vector<std::string>& out) {
    /// clear the response buffer now.
    cumulativeOptionNumber = 0;
    CoAPMessage coapmessage;
    auto ret = parseRequest(in, coapmessage);
    auto cf = getContentFormat(coapmessage);
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l processRequest with session cf.length: %d\n"),
               static_cast<int>(cf.length())));

    // L9 / FUP-2 — Acknowledgement short-circuit + dispatch to
    // RegistrationClient FSM. Same shape as the no-DTLS overload above.
    if (!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        if (isAmIClient && m_regClient) {
            m_regClient->on_response(coapmessage, *this);
        }
        return 0;
    }

    // L4 hot path: if a Bootstrap Server is attached, give it first
    // refusal on /bs CoAP messages.
    if (!isAmIClient && m_bsServer) {
        auto bsr = m_bsServer->handle(coapmessage, *this);
        if (bsr.handled) {
            for (auto& f : bsr.frames) out.push_back(std::move(f));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L3 hot path: if a RegistrationServer is attached, give it first
    // refusal on /rd-family URIs. It returns kind==None for non-/rd
    // requests; otherwise it produces the CoAP response and we are done.
    if (!isAmIClient && m_regServer && session != nullptr) {
        std::string peerHost(inet_ntoa(session->addr.sin.sin_addr));
        std::uint16_t peerPort = ntohs(session->addr.sin.sin_port);
        auto outcome = m_regServer->handle(coapmessage, *this, peerHost, peerPort);
        if (outcome.kind != ::lwm2m::RegistrationOutcome::None) {
            if (!outcome.response.empty()) out.push_back(std::move(outcome.response));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L5 hot path: client-side DM dispatcher. Handles /{oid}[/...] coming
    // from the registered LwM2M Server. Returns kind==None when the URI
    // isn't a numeric DM path.
    if (isAmIClient && m_dmClient) {
        auto dmOut = m_dmClient->handle(coapmessage, *this);
        if (dmOut.kind != ::lwm2m::DmOutcome::None) {
            if (!dmOut.response.empty()) out.push_back(std::move(dmOut.response));
            return static_cast<std::int32_t>(out.size());
        }
    }

    // L9 hot path: client-side Bootstrap dispatcher. Handles inbound
    // bootstrap-write / delete / finish coming from the BootstrapServer
    // during the bootstrap window. handle_bs_traffic returns an empty
    // string when the message is unrelated, so the caller falls through.
    if (isAmIClient && m_bsClient) {
        auto rsp = m_bsClient->handle_bs_traffic(coapmessage, *this);
        if (!rsp.empty()) {
            out.push_back(std::move(rsp));
            return static_cast<std::int32_t>(out.size());
        }
    }

    if(cf.length() > 0 && (cf == "application/vnd.oma.lwm2m+tlv") || (cf == "text/plain;charset=utf-8")) {

        ///Build the Response for a given Request
        std::string uri;
        std::uint32_t oid, oiid, rid, riid;
        if(isLwm2mUri(coapmessage, uri, oid, oiid, rid, riid)) {
            if("rd" == uri || "bs" == uri) {
                //rsp = buildResponse(coapmessage);
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l building an ACK\n")));
                std::string rsp = buildRegistrationAck(coapmessage);
                out.push_back(rsp);
                if(!uri.compare(0, 2, "bs") && !isAmIClient) {
                    ///@brief The bootstrap request
                    auto responses = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
                    for(auto& response: responses) {
                        out.push_back(response);
                    }
                } else {
                    ///@brief The registration or registration update
                }
            } else {
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l received LwM2M Object oid: %u oiid: %u\n"),
                           static_cast<unsigned>(oid),
                           static_cast<unsigned>(oiid)));
                out = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
            }
            /// This is LwM2M string URI rd or bs
            //out = handleLwM2MObjects(coapmessage, uri, oid, oiid, rid, riid);
            return(out.size());
        }

    } else if(!RequestType[coapmessage.coapheader.type].compare("Acknowledgement")) {
        ///response is a calls variable.
        //response = buildResponse(coapmessage);
    } else {

        ///This is a CoAP Request
        dumpCoAPMessage(coapmessage);
        std::string rsp = buildResponse(coapmessage);
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
                            ACE_DEBUG((LM_DEBUG,
                                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l object.m_oid:%u ent.m_oiid:%u ent.m_riid:%u ent.m_rid:%C ent.m_ridlength:%u ent.m_ridvalue.size:%d ent.m_ridvalue:\n"),
                                       static_cast<unsigned>(object.m_oid),
                                       static_cast<unsigned>(ent.m_oiid),
                                       static_cast<unsigned>(ent.m_riid),
                                       lwm2mAdapter.resourceIDName(oid, ent.m_rid).c_str(),
                                       static_cast<unsigned>(ent.m_ridlength),
                                       static_cast<int>(ent.m_ridvalue.size())));
        
                            {
                                std::string hex; char b[4];
                                for(const auto& elm: ent.m_ridvalue) {
                                    snprintf(b, sizeof(b), "%02X ", (std::uint8_t)elm);
                                    hex += b;
                                }
                                ACE_DEBUG((LM_DEBUG,
                                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l ridvalue(hex): %C\n"),
                                           hex.c_str()));
                            }
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
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l ver: %C type: %C tokenlength: %C code: %C msgid: %u\n"),
               std::to_string(coapmessage.coapheader.ver).c_str(),
               getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type)).c_str(),
               std::to_string(coapmessage.coapheader.tokenlength).c_str(),
               ss.str().c_str(),
               static_cast<unsigned>(coapmessage.coapheader.msgid)));

    for(auto const& opt: coapmessage.uripath) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l optiondelta: %C optionlength: %C optionvalue: %C\n"),
                   getOptionNumber(static_cast<std::uint32_t>(opt.optiondelta)).c_str(),
                   std::to_string(opt.optionlength).c_str(),
                   opt.optionvalue.c_str()));
    }

    auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
        return(getOptionNumber(ent.optiondelta).compare("Block1") == 0);
    });

    if(it != coapmessage.uripath.end()) {
        auto ent = *it;
        auto blk1 = static_cast<std::uint8_t>(ent.optionvalue.at(0));
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Block1 Length: %02X\n"), blk1));
        auto szx = blk1 & 0b111;
        auto m = (blk1 >> 3) & 0b1;
        auto num = (blk1 >> 4) & 0b1111;

        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Block Number: %C More: %C Block Size: %C\n"),
                   std::to_string(num).c_str(),
                   std::to_string(m).c_str(),
                   std::to_string(szx).c_str()));
    }
}









#endif /*__coap_adapter_cpp__*/