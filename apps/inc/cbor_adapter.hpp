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
        std::int32_t json2cbor(const std::string& in, std::string& cbor);
        std::string getJson(const std::string& fname);
        std::int32_t getCBOR(const std::string& fname, std::string& cbor);
        bool writeIntoFile(const std::string &input, const std::string& fileName);

    private:
        std::string data;
};




















#endif /*__cbor_adapter_hpp__*/