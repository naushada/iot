#ifndef __cbor_adapter_hpp__
#define __cbor_adapter_hpp__

#include <iostream>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

class CBORAdapter {
    public:
        CBORAdapter();
        ~CBORAdapter();
        /**
         * @brief This method creates json input into cbor representations
         * 
         * @param in json input
         * @param cbor output
         * @return std::int32_t upon success 0 else -1 
         */
        std::int32_t json2cbor(const std::string& in, std::string& cbor);
        /**
         * @brief Get the Json object from the json file
         * 
         * @param fname 
         * @return std::string 
         */
        std::string getJson(const std::string& fname);
        /**
         * @brief reads CBOR from a file provided by file name
         * 
         * @param fname 
         * @param cbor 
         * @return std::int32_t 
         */
        std::int32_t getCBOR(const std::string& fname, std::string& cbor);
        /**
         * @brief This method writes received data into file
         * 
         * @param input 
         * @param fileName 
         * @return true 
         * @return false 
         */
        bool writeIntoFile(const std::string &input, const std::string& fileName);

    private:
        std::string data;
};




















#endif /*__cbor_adapter_hpp__*/