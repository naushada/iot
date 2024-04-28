#ifndef __coap_adapter_hpp__
#define __coap_adapter_hpp__

#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>

#include "cbor_adapter.hpp"
#include "lwm2m_adapter.hpp"

extern "C" {
    #include "dtls.h"
    #define ZIP_DISABLE_DEPRECATED
    #include <zlib.h>
}

class CoAPAdapter {
    public:
        struct CoAPOptions {
            /// @brief This is same as type, This could be encoded in either 4bits, 8bits or 16bits
            std::uint32_t optiondelta;
            /// @brief This is type length, This could be encoded in either 4bits, 8bits or 16bits
            std::uint32_t optionlength;
            /// @brief This is value part of type
            std::string optionvalue;
        };
        struct CoAPHeader {
            std::uint8_t ver;
            std::uint8_t type;
            std::uint8_t tokenlength;
            std::uint8_t code;
            std::uint16_t msgid;
        };
        struct CoAPMessage {
            CoAPHeader coapheader;
            /// @brief  Token length could be between 0 and 8 bytes long.
            std::vector<std::uint8_t> tokens;
            std::vector<CoAPOptions> uripath;
            bool ismorebitset;
            std::string payload;
        };

        CoAPAdapter();
        ~CoAPAdapter();
        /**
         * @brief 
         * 
         * @param session 
         * @param in 
         * @return std::int32_t 
         */
        std::int32_t processRequest(bool isAmIClient, session_t *session, std::string &in, std::vector<std::string>& out);
        std::int32_t processRequest(bool isAmIClient, const std::string& in, std::vector<std::string>& out);
        /**
         * @brief 
         * 
         * @param in 
         * @param coapmessage 
         * @return std::int32_t 
         */
        std::int32_t parseRequest(const std::string& in, CoAPMessage& coapmessage);
        /**
         * @brief 
         * 
         * @param message 
         * @return std::string 
         */
        std::string buildResponse(const CoAPMessage& message);
        /**
         * @brief 
         * 
         * @param message 
         * @return std::string 
         */
        std::string buildRegistrationAck(const CoAPMessage& message);
        /**
         * @brief 
         * 
         * @param message 
         * @return std::string 
         */
        std::string buildPushContinue(const CoAPMessage& message);
        /**
         * @brief 
         * 
         * @param message 
         * @return std::string 
         */
        std::string buildPushAck(const CoAPMessage& message);
        /**
         * @brief 
         * 
         * @param input 
         * @param output 
         * @return true 
         * @return false 
         */
        bool uncompress(const std::string &input, std::string &output);
        /**
         * @brief 
         * 
         * @param input 
         * @param output 
         * @return true 
         * @return false 
         */
        bool compress(const std::string &input, std::string &output);
        /**
         * @brief 
         * 
         * @param input 
         * @param fileName 
         * @return true 
         * @return false 
         */
        bool writeIntoFile(const std::string &input, const std::string& fileName);
        /**
         * @brief 
         * 
         * @param input 
         * @param request 
         * @return true 
         * @return false 
         */
        bool buildRequest(const std::string &input, std::vector<std::string>& request);
        /**
         * @brief 
         * 
         * @param uri 
         * @param query 
         * @param request 
         * @param cf 
         * @param out 
         * @return true 
         * @return false 
         */
        bool serialisePOST(const std::vector<std::string> &uri, const std::vector<std::string> &query, const std::vector<std::string>& request,
                           const std::uint16_t& cf, std::vector<std::string>& out);
        /**
         * @brief 
         * 
         * @param uri 
         * @param query 
         * @param request 
         * @param cf 
         * @param method 
         * @param out 
         * @return true 
         * @return false 
         */
        bool serialise(const std::vector<std::string> &uri, const std::vector<std::string> &query, const std::vector<std::string>& request,
                           const std::uint16_t& cf, const std::uint8_t& method, std::vector<std::string>& out);

        std::string getRequestType(std::uint32_t type) {
            return(RequestType[type]);
        }

        std::string getMethodCode(std::uint32_t code) {
            return(MethodCode[code]);
        }

        std::uint32_t getMethodCode(std::string code) {
            return(MethodCodeByName[code]);
        }
        std::string getResponseCode(std::string statuscode) {
            return(ResponseCode[statuscode]);
        }

        std::string getUriStr(std::uint32_t opt) {
            return(getOptionNumber(opt));
        }

        std::string getOptionNumber(std::uint32_t opt) {
            return(OptionNumber[opt]);
        }

        std::string getContentFormat(std::uint32_t cf) {
            return(ContentFormat[cf]);
        }

        std::uint32_t getContentFormat(std::string cf) {
            return(ContentFormatByName[cf]);
        }
        std::uint32_t SumOptionNumber(std::uint32_t optnumber) {
            cumulativeOptionNumber += optnumber;
            return(cumulativeOptionNumber);
        }

        void resetCummulativeOptionNumber() {
            cumulativeOptionNumber = 0;
        }
        
        void dumpCoAPMessage(const CoAPMessage& coapMessage);

        std::string getResponse() {
            return(response);
        }

        CBORAdapter& getCBORAdapter() {
            return(cborAdapter);
        }

        bool isLwm2mUri(const CoAPMessage& message, std::string& uriName, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid, std::uint32_t& riid) {
            auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {
                return((ent.optionvalue == "rd") || (ent.optionvalue == "bs"));
            });

            if(it != message.uripath.end()) {
                auto &ent = *it;
                uriName = ent.optionvalue;
                return(true);
            }

            return(isLwm2mUriObject(message, oid, oiid, rid, riid));
        }

        bool isLwm2mUriObject(const CoAPMessage& message, std::uint32_t& oid, std::uint32_t& oiid, std::uint32_t& rid, std::uint32_t& riid) {
            bool ret = false;
                        
            if(!message.uripath.empty() && message.uripath.size() > 0 && !getUriStr(message.uripath.at(0).optiondelta).compare("Uri-Path") &&
                std::isdigit(message.uripath.at(0).optionvalue.at(0))) {
                oid = std::stoi(message.uripath.at(0).optionvalue);
                ret = true;
            }
            
            if(!message.uripath.empty() && message.uripath.size() > 1 && !getUriStr(message.uripath.at(1).optiondelta).compare("Uri-Path") &&
            std::isdigit(message.uripath.at(1).optionvalue.at(0))) {
                oiid = std::stoi(message.uripath.at(1).optionvalue);
                ret = true;
            }
            
            if(!message.uripath.empty() && message.uripath.size() > 2 && !getUriStr(message.uripath.at(2).optiondelta).compare("Uri-Path") &&
            std::isdigit(message.uripath.at(2).optionvalue.at(0))) {
                rid = std::stoi(message.uripath.at(2).optionvalue);
            }

            if(!message.uripath.empty() && message.uripath.size() > 3 && !getUriStr(message.uripath.at(3).optiondelta).compare("Uri-Path") &&
            std::isdigit(message.uripath.at(3).optionvalue.at(0))) {
                riid = std::stoi(message.uripath.at(3).optionvalue);
            }
            
            return(ret);
        }

        bool isCoAPUri(const CoAPMessage& message, std::string& uri) {
            auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {
                auto iter = std::find_if(CoAPUri.begin(), CoAPUri.end(), [&](const auto& elm) -> bool {
                    return(ent.optionvalue == elm);
                });
                return(iter != CoAPUri.end());
            });

            if(it != message.uripath.end()) {
                auto &ent = *it;
                uri = ent.optionvalue;
                return(true);
            }

            return(false);
        }

        std::string getEndPoint(const CoAPMessage& message) {
            auto it = std::find_if(message.uripath.begin(), message.uripath.end(), [&](const auto& ent) -> bool {
                return(!OptionNumber[ent.optiondelta].compare(0, 9,"Uri-Query") && (!ent.optionvalue.compare(0, 2, "ep")));
            });

            if(it != message.uripath.end()) {
                auto &elm = *it;
                /// @brief returns the serialnumber, i.e. ep="Asder123456"
                return(elm.optionvalue.substr(3));
            }

            return(std::string());
        }

        std::string getContentFormat(const CoAPMessage& coapmessage ) {
            auto it = std::find_if(coapmessage.uripath.begin(), coapmessage.uripath.end(), [&](const auto& ent) -> bool {
                return(OptionNumber[ent.optiondelta] == "Content-Format");
            });

            if(it != coapmessage.uripath.end()) {
                std::cout << basename(__FILE__) << ":" << __LINE__ << " cf: " << atoi(it->optionvalue.c_str()) << std::endl;
                return(getContentFormat(atoi(it->optionvalue.c_str())));
            }

            return(getContentFormat(0));
        }

        std::shared_ptr<LwM2MAdapter>& lwm2mAdapter() {
            return(m_lwm2mAdapter);
        }

        std::vector<std::string> handleLwM2MObjects(const CoAPAdapter::CoAPMessage& message, std::string uri, std::uint32_t oid, 
                                                    std::uint32_t oiid, std::uint32_t rid, std::uint32_t riid);

    private:
        std::unordered_map<std::uint32_t, std::string> OptionNumber;
        std::unordered_map<std::uint32_t, std::string> ContentFormat;
        std::unordered_map<std::string, std::uint32_t> ContentFormatByName;
        std::unordered_map<std::string, std::string> ResponseCode;
        std::unordered_map<std::uint32_t, std::string> MethodCode;
        std::unordered_map<std::string,  std::uint32_t> MethodCodeByName;
        std::unordered_map<std::uint32_t, std::string> RequestType;
        std::vector<std::string> CoAPUri;
        std::string response;
        std::uint32_t cumulativeOptionNumber;
        CBORAdapter cborAdapter;
        std::shared_ptr<LwM2MAdapter> m_lwm2mAdapter;

};




#endif /*__coap_adapter_hpp__*/