#ifndef __coap_adapter_hpp__
#define __coap_adapter_hpp__

#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>

#include "cbor_adapter.hpp"

extern "C" {
    #include "dtls.h"
    #define ZIP_DISABLE_DEPRECATED
    #include <zlib.h>
}

class CoAPAdapter {
    public:
        struct CoAPOptions {
            std::uint32_t optiondelta;
            std::uint32_t optionlength;
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
        std::int32_t processRequest(session_t *session, std::string &in);
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

        std::string getResponseCode(std::string statuscode) {
            return(ResponseCode[statuscode]);
        }

        std::string getOptionNumber(std::uint32_t opt) {
            return(OptionNumber[opt]);
        }

        std::string getContentFormat(std::uint32_t cf) {
            return(ContentFormat[cf]);
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

    private:
        std::unordered_map<std::uint32_t, std::string> OptionNumber;
        std::unordered_map<std::uint32_t, std::string> ContentFormat;
        std::unordered_map<std::string, std::string> ResponseCode;
        std::unordered_map<std::uint32_t, std::string> MethodCode;
        std::unordered_map<std::uint32_t, std::string> RequestType;
        std::string response;
        std::uint32_t cumulativeOptionNumber;
        CBORAdapter cborAdapter;

};




#endif /*__coap_adapter_hpp__*/