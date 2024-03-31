#ifndef __main_cpp__
#define __main_cpp__

#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "app.hpp"
#include "readline.hpp"


std::unordered_map<std::string, UDPAdapter::Scheme_t> schemeMap = {
    {"coaps", UDPAdapter::Scheme_t::CoAPs},
    {"coap", UDPAdapter::Scheme_t::CoAP}
};

std::unordered_map<std::string, UDPAdapter::Role_t> roleMap = {
    {"server", UDPAdapter::Role_t::SERVER},
    {"client", UDPAdapter::Role_t::CLIENT}
};

void parseCommandLineArgument(std::int32_t argc, char *argv[], std::unordered_map<std::string, std::string>& out) {

    if(argc > 1) {
        size_t idx = 1;

        while(argv[idx] != NULL) {

            std::string arg(argv[idx], strlen(argv[idx]));
            std::istringstream iss(arg);
            std::ostringstream key, value;

            if(!iss.get(*key.rdbuf(), '=').eof()) {
                key.str().resize(iss.gcount());
                iss.get();
                iss.get(*value.rdbuf(), ' ');
                out[key.str()] = value.str();
            }

            ++idx;

        }
    }
    
}

void parsePeerOption(const std::string& in, UDPAdapter::Scheme_t& scheme, std::string& host, std::uint16_t& port) {
    ///in = coaps://host:port
    if(in.empty()) {
        ///input is empty, 
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error input is empty" << std::endl;
        return;
    }

    std::istringstream iss(in);
    std::ostringstream value;

    if(!iss.get(*value.rdbuf(), ':').eof()) {
        value.str().resize(iss.gcount());
        scheme = schemeMap[value.str()];
        /// get rid of ":" now
        iss.get();
    }

    /// get rid of "//"
    iss.get();
    iss.get();

    value.str("");
    if(!iss.get(*value.rdbuf(), ':').eof()) {
        value.str().resize(iss.gcount());
        host = value.str();
        /// get rid of ":" now
        iss.get();
    }
    
    value.str("");
    if(iss.get(*value.rdbuf()).eof()) {
        value.str().resize(iss.gcount());
        port = 0;
        if(value.str().length() > 0) {
            port = std::stod(value.str());
        }
    }
}

int main(std::int32_t argc, char *argv[]) {

    std::unordered_map<std::string, std::string> argValueMap;
    parseCommandLineArgument(argc, argv, argValueMap);
    if(!argValueMap.empty()) {

        for(const auto& ent: argValueMap) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " ent.first:" << ent.first << " ent.second:" << ent.second << std::endl;
        }

    }

    UDPAdapter::Role_t role = UDPAdapter::Role_t::SERVER;
    if(!argValueMap["role"].empty() && (argValueMap["role"] == "server" || argValueMap["role"] == "client")) {
        role = roleMap[argValueMap["role"]];
    } else {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error Invalid Option for role" << std::endl;
        return(-1);
    }

    UDPAdapter::Scheme_t scheme;
    /// Bootstrap Host & Port
    std::string bsHost;
    std::uint16_t bsPort;
    if(UDPAdapter::Role_t::CLIENT == role) {
        if(argValueMap["bs"].empty()) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error bs=value is missing in command line argument" << std::endl;
            return(-1);
        }
        parsePeerOption(argValueMap["bs"], scheme, bsHost, bsPort);
    }

    std::string selfHost;
    std::uint16_t selfPort;
    if(argValueMap["local"].empty()) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error local=value is missing in command line argument" << std::endl;
        return(-1);
    }
    parsePeerOption(argValueMap["local"], scheme, selfHost, selfPort);

    std::cout << basename(__FILE__) << ":" << __LINE__ << " scheme:" << std::to_string(scheme) << " host:" << selfHost << " port:" << std::to_string(selfPort) << std::endl;

    

    std::string identity("97554878B284CE3B727D8DD06E87659A"), secret("3894beedaa7fe0eae6597dc350a59525");
    if(scheme == UDPAdapter::Scheme_t::CoAPs) {
        ///identity & secret are mandatory argument
        if(argValueMap["identity"].empty() || argValueMap["secret"].empty()) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error either identity or secret missing for coaps" << std::endl;
            return(-2);
        }

        identity.assign(argValueMap["identity"]);
        secret.assign(argValueMap["secret"]);
    }

    UDPAdapter::ServiceType_t service;
    if(UDPAdapter::Role_t::CLIENT == role) {
        service = UDPAdapter::ServiceType_t::LwM2MClient;
    } else {
        service = UDPAdapter::ServiceType_t::BootsstrapServer;
    }

    std::shared_ptr<App> app = std::make_shared<App>(selfHost, selfPort, scheme, service);
    app->udpAdapter()->add_event_handle(scheme, service);
    
    if(UDPAdapter::Role_t::SERVER == role) {
        app->udpAdapter()->init(selfHost, 58686, scheme, UDPAdapter::ServiceType_t::DeviceMgmtServer);
        app->udpAdapter()->add_event_handle(scheme, UDPAdapter::ServiceType_t::DeviceMgmtServer);

    } else {
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            ent.second->peerHost(bsHost);
            ent.second->peerPort(bsPort);
        }
    }

    if(scheme == UDPAdapter::Scheme_t::CoAPs) {
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            ent.second->dtlsAdapter()->add_credential(identity, secret);
        }
    }

    // install signal handler
    struct sigaction sa;
    //sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGPIPE,  &sa, NULL);

    // prepare signal masks
    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGTERM);
    sigaddset(&signalMask, SIGPIPE);
    sigset_t emptyMask;
    sigemptyset(&emptyMask);

    if(UDPAdapter::Role_t::CLIENT == role) {
        std::thread reception_thread(&App::start, &(*app), role, scheme);
        Readline rline(app);
        rline.init();
        rline.start();
    } else {
        app->start(role, scheme);
    }

    return(0);
}





#endif /* __main_cpp__*/