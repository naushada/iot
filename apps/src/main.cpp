#ifndef __main_cpp__
#define __main_cpp__

#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "app.hpp"
#include "readline.hpp"

#include "lwm2m_bootstrap.hpp"
#include "lwm2m_bootstrap_client.hpp"
#include "lwm2m_bootstrap_server.hpp"
#include "lwm2m_codec_tlv.hpp"
#include "lwm2m_dm_client.hpp"
#include "lwm2m_object_3_device.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_object_stubs.hpp"
#include "lwm2m_registration.hpp"
#include "lwm2m_registration_client.hpp"
#include "lwm2m_registration_server.hpp"

#include "nlohmann/json.hpp"


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

/* ───────────────────────── L9 wiring helpers ─────────────────────────── */

namespace {

using json = nlohmann::json;

/// Read the existing security/server-object JSON files under
/// `apps/config/{securityObject,serverObject}/` and synthesise one
/// AccountProvisioning per Security Object instance. Only instances
/// flagged as DM accounts (`isBootstrapServer` = false in the rid=1
/// override) get a paired Server Object.
///
/// The endpoint name is provided by the caller — typically passed via
/// `ep=` on the server's CLI, or hard-coded for a known test client.
::lwm2m::bootstrap::AccountProvisioning
load_provisioning_from_config(const std::string& configDir,
                              const std::string& endpoint) {
    ::lwm2m::bootstrap::AccountProvisioning a;
    a.endpoint = endpoint;

    auto loadFile = [&](const std::string& path, json& doc) -> bool {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;
        try { ifs >> doc; } catch (...) { return false; }
        return doc.is_array();
    };

    auto stringRid = [](const json& arr, std::uint32_t rid,
                        const std::string& def) -> std::string {
        for (const auto& rec : arr) {
            if (!rec.is_object() || !rec.contains("rid")) continue;
            if (rec["rid"].get<std::uint32_t>() != rid) continue;
            if (!rec.contains("value") || !rec["value"].is_string()) return def;
            return rec["value"].get<std::string>();
        }
        return def;
    };
    auto intRid = [](const json& arr, std::uint32_t rid,
                     std::uint32_t def) -> std::uint32_t {
        for (const auto& rec : arr) {
            if (!rec.is_object() || !rec.contains("rid")) continue;
            if (rec["rid"].get<std::uint32_t>() != rid) continue;
            if (!rec.contains("value") || !rec["value"].is_number_unsigned()) return def;
            return rec["value"].get<std::uint32_t>();
        }
        return def;
    };
    auto boolRid = [](const json& arr, std::uint32_t rid,
                      bool def) -> bool {
        for (const auto& rec : arr) {
            if (!rec.is_object() || !rec.contains("rid")) continue;
            if (rec["rid"].get<std::uint32_t>() != rid) continue;
            if (!rec.contains("value") || !rec["value"].is_boolean()) return def;
            return rec["value"].get<bool>();
        }
        return def;
    };

    // Security Object instances: file naming mirrors today's "0.json", "1.json"
    // and the loaded order maps directly to the wire iid.
    for (std::uint16_t iid : {0, 1}) {
        json doc;
        std::string p = configDir + "/securityObject/" +
                        std::to_string(iid) + ".json";
        if (!loadFile(p, doc)) continue;

        ::lwm2m::bootstrap::SecurityInstance s;
        s.iid               = iid;
        s.serverUri         = stringRid(doc, 0, "");
        s.isBootstrapServer = boolRid(doc, 1, iid == 0);
        s.securityMode      = static_cast<std::uint8_t>(intRid(doc, 2, 3));
        s.identity          = stringRid(doc, 3, "");
        s.secretKey         = stringRid(doc, 5, "");
        s.shortServerId     = static_cast<std::uint16_t>(intRid(doc, 10, iid + 1));
        a.security.push_back(std::move(s));
    }

    // Server Object instance 0 is the canonical DM-server account.
    json srvDoc;
    if (loadFile(configDir + "/serverObject/0.json", srvDoc)) {
        ::lwm2m::bootstrap::ServerInstance srv;
        srv.iid           = 0;
        srv.shortServerId = static_cast<std::uint16_t>(intRid(srvDoc, 0, 1));
        srv.lifetime      = intRid(srvDoc, 1, 86400);
        srv.binding       = stringRid(srvDoc, 7, "U");
        a.server.push_back(std::move(srv));
    }
    return a;
}

/// Attach BootstrapServer (on the Bootstrap port) + RegistrationServer +
/// canonical ObjectStore (for Discover output) on the DeviceMgmtServer
/// port. Install the 1 Hz tick to drive registry expiry.
void wire_server(std::shared_ptr<App>& app, const std::string& configDir) {
    auto registry = std::make_shared<::lwm2m::ClientRegistry>();
    auto bsServer = std::make_shared<::lwm2m::bootstrap::Server>();

    // Provision one account for the well-known test endpoint. Real
    // deployments would load multiple accounts from a registry file or
    // the DB; the wiring point stays the same.
    bsServer->add_account(
        load_provisioning_from_config(configDir, "urn:dev:client-1"));

    auto& services = app->udpAdapter()->services();
    for (auto& [type, ctx] : services) {
        if (type == ::UDPAdapter::ServiceType_t::BootsstrapServer) {
            ctx->coapAdapter()->bootstrapServer(bsServer);
        }
        if (type == ::UDPAdapter::ServiceType_t::DeviceMgmtServer) {
            ctx->coapAdapter()->registrationServer(
                std::make_shared<::lwm2m::RegistrationServer>(registry));
        }
    }

    app->udpAdapter()->on_tick_server([registry]() {
        auto now = std::chrono::steady_clock::now();
        auto expired = registry->expire(now);
        if (!expired.empty()) {
            std::cout << "[lwm2m] expired " << expired.size()
                      << " registration(s)\n";
        }
        // L3 follow-up: forward expired locations to the RegistryMirror
        // when the worker is wired in.
    });
}

/// Build the client's ObjectStore + handlers and attach them to the
/// LwM2MClient ServiceContext. Returns the BootstrapClient + the
/// RegistrationClient so the caller can drive the FSM.
struct ClientPlumbing {
    std::shared_ptr<::lwm2m::ObjectStore>            store;
    std::shared_ptr<::lwm2m::DmClient>               dm;
    std::shared_ptr<::lwm2m::bootstrap::Client>      bs;
    std::shared_ptr<::lwm2m::RegistrationClient>     reg;
};

ClientPlumbing wire_client(std::shared_ptr<App>& app,
                           const std::string& endpoint,
                           const std::string& configDir,
                           const std::string& bsHost,
                           std::uint16_t bsPort) {
    ClientPlumbing plumb;
    plumb.store = std::make_shared<::lwm2m::ObjectStore>();
    ::lwm2m::objects::install_canonical_objects(*plumb.store, configDir);

    plumb.dm = std::make_shared<::lwm2m::DmClient>(plumb.store);
    plumb.dm->calling_peer(bsHost + ":" + std::to_string(bsPort));

    // The BootstrapClient gets a handle to the DTLS adapter so it can
    // install PSK credentials at commit time; for plain-CoAP runs the
    // ServiceContext returns a null dtlsAdapter and we pass that.
    auto& services = app->udpAdapter()->services();
    std::shared_ptr<DTLSAdapter> dtls;
    for (auto& [type, ctx] : services) {
        if (type == ::UDPAdapter::ServiceType_t::LwM2MClient) {
            dtls = ctx->dtlsAdapter();
        }
    }

    plumb.bs = std::make_shared<::lwm2m::bootstrap::Client>(
        endpoint, plumb.store, dtls);

    ::lwm2m::ClientConfig cfg;
    cfg.endpoint     = endpoint;
    cfg.lifetime     = 86400;
    cfg.binding      = "U";
    cfg.lwm2mVersion = "1.1";
    plumb.reg = std::make_shared<::lwm2m::RegistrationClient>(cfg, *plumb.store);

    for (auto& [type, ctx] : services) {
        if (type != ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
        ctx->coapAdapter()->dmClient(plumb.dm);
        ctx->coapAdapter()->bootstrapClient(plumb.bs);
    }

    // On bootstrap completion, log + leave the reg client ready. The
    // actual Register POST is triggered by the next on_tick_client
    // call so we don't block the reactor thread inside the on_done
    // callback.
    plumb.bs->on_done([](const ::lwm2m::bootstrap::StagingBuffer&) {
        std::cout << "[lwm2m] bootstrap commit done; registration pending\n";
    });

    // 1 Hz ticker: drives Update emission + Observe pmax + (TODO) initial
    // Register once bootstrap completes.
    std::weak_ptr<::lwm2m::RegistrationClient> wreg = plumb.reg;
    std::weak_ptr<::lwm2m::DmClient>           wdm  = plumb.dm;
    std::weak_ptr<App>                         wapp = app;
    app->udpAdapter()->on_tick_client([wreg, wdm, wapp]() {
        auto reg = wreg.lock();
        auto dm  = wdm.lock();
        auto a   = wapp.lock();
        if (!reg || !dm || !a) return;

        auto now = std::chrono::steady_clock::now();
        if (reg->should_send_update(now)) {
            std::cout << "[lwm2m] registration update due\n";
            // Caller code would build_update_request + tx via UDPAdapter::tx
            // here; left for the L9 follow-up that observes Leshan timing.
        }
        // Observe pmax ticker.
        auto frames = dm->tick(now);
        if (!frames.empty()) {
            std::cout << "[lwm2m] notify burst: " << frames.size() << " frames\n";
            // ship via UDPAdapter::tx on the LwM2MClient service.
        }
    });

    return plumb;
}

} // namespace

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
        app->udpAdapter()->init(selfHost, 5683, scheme, UDPAdapter::ServiceType_t::DeviceMgmtServer);
        app->udpAdapter()->add_event_handle(scheme, UDPAdapter::ServiceType_t::DeviceMgmtServer);
    } else {
        /// @brief role is client
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            ent.second->peerHost(bsHost);
            ent.second->peerPort(bsPort);
        }
    }

    if(UDPAdapter::Scheme_t::CoAPs == scheme) {
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            ent.second->dtlsAdapter()->add_credential(identity, secret);
        }
    }

    // L9 wiring: instantiate the LwM2M handlers + ObjectStore and bind
    // them to the relevant ServiceContext_t CoAPAdapters. configDir
    // defaults to "../config" so the existing apps/config/ JSON files
    // (security, server, device) are picked up when the binary runs
    // from apps/build/.
    const std::string configDir = !argValueMap["config"].empty()
                                      ? argValueMap["config"]
                                      : std::string("../config");
    const std::string endpoint  = !argValueMap["ep"].empty()
                                      ? argValueMap["ep"]
                                      : std::string("urn:dev:client-1");

    ClientPlumbing clientPlumbing;
    if (UDPAdapter::CLIENT == role) {
        clientPlumbing = wire_client(app, endpoint, configDir, bsHost, bsPort);
    } else {
        wire_server(app, configDir);
    }

    // UDP socket churn (peer torn down between writes) would otherwise
    // raise SIGPIPE and kill the process; ignore it. SIGINT/SIGTERM are
    // handled by the ACE reactor loop inside UDPAdapter::start().
    ::signal(SIGPIPE, SIG_IGN);

    if(UDPAdapter::CLIENT == role) {
        // App::start activates the reactor on its own ACE_Task thread and
        // returns immediately so the main thread can drive readline.
        app->start(role, scheme);
        Readline rline(app);
        rline.init();
        rline.start();
        app->stop();
    } else {
        // Server: reactor runs on the main thread inside App::start.
        app->start(role, scheme);
    }

    return(0);
}





#endif /* __main_cpp__*/