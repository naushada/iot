#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"

std::int32_t App::start(UDPAdapter::Role_t role, UDPAdapter::Scheme_t scheme) {
    udpAdapter()->start(role, scheme);
    return(0);
}

std::int32_t App::stop() {
    return(0);
}

std::int32_t App::init(const std::string& bsFile) {
    std::ifstream ifs(bsFile);
    std::stringstream ss("");

    if(ifs.is_open()) {
        ss << ifs.rdbuf();
        ifs.close();

        json bs = json::parse(ss.str());
        for(const auto& ent: bs) {
            for(const auto& [key, value]: ent.items()) {
                if(!key.compare("serial_no") && value.is_string()) {
                    std::shared_ptr<App::Device> dev;
                    device(value.get<std::string>(), dev);
                }
            }
        }
        return(0);
    }
    return(-1);

}

#endif /*__app_cpp__*/