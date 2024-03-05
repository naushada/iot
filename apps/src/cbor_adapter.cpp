#ifndef __cbor_adapter_cpp__
#define __cbor_adapter_cpp__

#include "cbor_adapter.hpp"



CBORAdapter::CBORAdapter() {

}

CBORAdapter::~CBORAdapter() {

}

std::int32_t CBORAdapter::json2cbor(const std::string& in, std::string& out) {
    std::int32_t ret = 0;
    auto ents = json::parse(in);
    auto cbor = json::to_cbor(ents);
    out = std::string(cbor.begin(), cbor.end());
    return(ret);
}

std::string CBORAdapter::getJson(const std::string& fileName) {
    std::ifstream ifs;
    ifs.open(fileName, std::fstream::in);
    std::vector<char> result(1024);
    std::stringstream ss;

    if(!ifs.is_open()) {
        std::cout << basename(__FILE__) << ":" << " Error Opening of file: " << fileName << " is Failed" << std::endl;
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
        std::cout << basename(__FILE__) << ":" << " Unable to get the contents of json: " << fname << " is Failed" << std::endl;
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
        std::cout << basename(__FILE__) << ":" << " Opening of file: " << fileName << " is Failed" << std::endl;
        return(false);
    }

    ofs << input;
    ofs.close();
    return(true);
}






#endif /*__cbor_adapter_cpp__*/