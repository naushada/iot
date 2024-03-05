#ifndef __coap_adapter_cpp__
#define __coap_adapter_cpp__

#include "coap_adapter.hpp"

CoAPAdapter::CoAPAdapter() {
    cumulativeOptionNumber = 0;
    response.clear();
    
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

    RequestType = {
        {0, "Confirmable"},
        {1, "Non-COnfirmable"},
        {2, "Acknowledgement"},
        {3, "Rest"}
    };
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
        }
        
        ss.write (reinterpret_cast <const char *>(request.data()), request.length());
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
    auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {return(ent.optionvalue == "rd");});
    if(it != message.uripath.end()) {
        /// This is a /rd -- registration request
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

    //dumpCoAPMessage(coapmessage);
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

std::int32_t CoAPAdapter::processRequest(session_t* session, std::string& in) {
    /// clear the response buffer now.
    cumulativeOptionNumber = 0;
    CoAPMessage coapmessage;
    auto ret = parseRequest(in, coapmessage);
    ///response is a calls variable.
    response = buildResponse(coapmessage);

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
            }
        }        
    }

    return(response.length());
}

void CoAPAdapter::dumpCoAPMessage(const CoAPMessage& coapmessage) {

    std::cout << std::endl << basename(__FILE__) << ":" << __LINE__ 
              << " ver: "         << std::to_string(coapmessage.coapheader.ver)
              << " type: "        << getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type))
              << " tokenlength: " << std::to_string(coapmessage.coapheader.tokenlength)
              << " code: "        << getMethodCode(static_cast<std::uint32_t>(coapmessage.coapheader.code))
              << " msgid: "       << std::hex << coapmessage.coapheader.msgid << std::dec << std::endl;

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