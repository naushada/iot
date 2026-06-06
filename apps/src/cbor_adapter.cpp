#ifndef __cbor_adapter_cpp__
#define __cbor_adapter_cpp__

#include "cbor_adapter.hpp"

#include <ace/Log_Msg.h>

CBORAdapter::CBORAdapter() {

}

CBORAdapter::~CBORAdapter() {

}

std::int32_t CBORAdapter::json2cbor(const std::string& in, std::string& out) {
    // BUG-002 side fix: nlohmann::json::parse throws on malformed input.
    // The original code propagated the throw up through readline, killing
    // the process (log.txt:9-11). Now we catch and return non-zero so the
    // caller can surface the error and the request is simply not built.
    out.clear();
    try {
        auto ents = json::parse(in);
        auto cbor = json::to_cbor(ents);
        out.assign(cbor.begin(), cbor.end());
        return 0;
    } catch (const std::exception& e) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l json2cbor parse error: %C\n"),
                   e.what()));
        return -1;
    }
}

std::string CBORAdapter::getJson(const std::string& fileName) {
    std::ifstream ifs;
    ifs.open(fileName, std::fstream::in);
    std::vector<char> result(1024);
    std::stringstream ss;

    if(!ifs.is_open()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l failed to open file: %C\n"),
                   fileName.c_str()));
        return(std::string());
    }

    while(!ifs.read(reinterpret_cast<char *>(result.data()), 1024).eof()) {
        result.resize(ifs.gcount());
        ss << std::string(result.begin(), result.end());
    }

    if(ifs.gcount()) {
        result.resize(ifs.gcount());
        ss << std::string(result.begin(), result.end());
    }

    //std::cout << basename(__FILE__) << ":" << __LINE__ << " The Content: " << ss.str() << std::endl;
    ifs.close();
    return(ss.str());
}

std::int32_t CBORAdapter::getCBOR(const std::string& fname, std::string& cbor) {
    auto data = getJson(fname);
    if(!data.length()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l unable to read JSON contents from %C\n"),
                   fname.c_str()));
        return(-1);
    }
    auto ret = json2cbor(data, cbor);
    writeIntoFile(cbor, "json2cbor.txt");
    return(0);
}

bool CBORAdapter::writeIntoFile(const std::string &input, const std::string& fileName) {

    std::ofstream ofs;
    ofs.open(fileName);

    if(!ofs.is_open()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l failed to open file for write: %C\n"),
                   fileName.c_str()));
        return(false);
    }

    ofs << input;
    ofs.close();
    return(true);
}






#endif /*__cbor_adapter_cpp__*/