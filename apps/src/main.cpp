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

#include <atomic>
#include "lwm2m_dm_server.hpp"

namespace {

using json = nlohmann::json;

/// Monotonic CoAP message-id allocator shared by every send site. CoAP
/// §4.4 requires uniqueness within the session window; a 16-bit counter
/// wrapping after ~64K outgoing messages is well within spec.
std::atomic<std::uint16_t> g_next_msgid{0x1000};
inline std::uint16_t next_msgid() { return ++g_next_msgid; }

/// Convenience tx wrapper — UDPAdapter::tx takes non-const lvalue refs
/// for historical reasons, so callers need locals.
inline void tx_via(App& app,
                   const std::string& payload,
                   ::UDPAdapter::ServiceType_t serviceType) {
    auto local = payload;                       // need non-const lvalue
    auto svc   = serviceType;
    app.udpAdapter()->tx(local, svc);
}

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

    // L9 stub 4 — periodic server-side DM driver. Once per
    // `kPollInterval`, walk the registry and issue a Read /3/0/0
    // (Manufacturer) against each registered client. This exercises
    // dmsrv::build_read going out through the existing DeviceMgmtServer
    // ServiceContext_t outbound queue. A real REPL is a follow-up;
    // this prosaic poll is enough to prove the wiring works in
    // Leshan interop pcaps.
    using Clock = std::chrono::steady_clock;
    auto last_poll = std::make_shared<Clock::time_point>(Clock::now());
    std::weak_ptr<App> wapp_srv = app;

    app->udpAdapter()->on_tick_server([registry, wapp_srv, last_poll]() {
        // Local constexpr inside the lambda body — gcc 11 wouldn't let
        // us reference an outer-scope constexpr without an explicit
        // capture even though it's a constant expression.
        constexpr auto kPollInterval = std::chrono::seconds(30);

        auto a = wapp_srv.lock();
        if (!a) return;
        const auto now = Clock::now();

        auto expired = registry->expire(now);
        if (!expired.empty()) {
            std::cout << "[lwm2m] expired " << expired.size()
                      << " registration(s)\n";
            // L3 follow-up: forward expired locations to the RegistryMirror
            // when the Mongo schema PR lands.
        }

        if (now - *last_poll < kPollInterval) return;
        *last_poll = now;

        // Walk services to find the DeviceMgmtServer ServiceContext;
        // its send_async takes an explicit peer so one outbound socket
        // serves every registered client.
        for (auto& kv : a->udpAdapter()->services()) {
            if (kv.first != ::UDPAdapter::ServiceType_t::DeviceMgmtServer) continue;
            auto& ctx = kv.second;
            for (const auto& reg_kv : registry->all()) {
                const auto& reg = reg_kv.second;
                if (reg.peerHost.empty()) continue;
                std::string token{static_cast<char>(0x03)};
                auto req = ::lwm2m::dmsrv::build_read(
                    next_msgid(), token,
                    /*oid*/ 3, /*iid*/ 0, /*rid*/ 0,
                    /*accept*/ -1);
                ctx->send_async(req, reg.peerHost, reg.peerPort);
            }
        }
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

    // L9 stub 1 — Register POST after bootstrap commit. Runs on the
    // reactor thread (handle_bs_traffic invoked it); the tx itself
    // queues via send_async + notify so we don't recurse into the I/O
    // path inside the callback.
    std::weak_ptr<App>                          wapp_bs = app;
    std::weak_ptr<::lwm2m::RegistrationClient>  wreg_bs = plumb.reg;
    plumb.bs->on_done([wapp_bs, wreg_bs](const ::lwm2m::bootstrap::StagingBuffer&) {
        auto a = wapp_bs.lock();
        auto r = wreg_bs.lock();
        if (!a || !r) return;
        std::cout << "[lwm2m] bootstrap commit done; sending Register\n";
        auto payload = r->build_register_request(next_msgid(),
                                                 std::string{static_cast<char>(0x01)});
        tx_via(*a, payload, ::UDPAdapter::ServiceType_t::LwM2MClient);
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

        const auto svc = ::UDPAdapter::ServiceType_t::LwM2MClient;
        const auto now = std::chrono::steady_clock::now();

        // L9 stub 2 — Update POST when the lifetime margin elapses.
        if (reg->should_send_update(now)) {
            auto payload = reg->build_update_request(
                next_msgid(),
                std::string{static_cast<char>(0x02)},
                /*withAdvertisedSet*/ false);
            tx_via(*a, payload, svc);
            reg->note_update_sent(now);
        }

        // L9 stub 3 — Notify dispatch. DmClient::tick returns ready-to-
        // ship CoAP frames (NON by default, every-10th CON per D4);
        // we just ship each in order.
        auto frames = dm->tick(now);
        for (auto& frame : frames) {
            tx_via(*a, frame, svc);
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

        // Readline requires a TTY on stdin. In container/CI runs stdin
        // is typically /dev/null (`-d` detached mode), so reading would
        // hit EOF immediately and the process would exit before any
        // bootstrap/register traffic completes. Detect that and just
        // park the main thread on the reactor task instead.
        if (::isatty(STDIN_FILENO)) {
            Readline rline(app);
            rline.init();
            rline.start();
        } else {
            std::cout << "[lwm2m] stdin is not a TTY; running in "
                      << "non-interactive mode (reactor only).\n";
            // ACE_Task::wait() blocks until svc() returns.
            app->udpAdapter()->wait();
        }
        app->stop();
    } else {
        // Server: reactor runs on the main thread inside App::start.
        app->start(role, scheme);
    }

    return(0);
}





#endif /* __main_cpp__*/