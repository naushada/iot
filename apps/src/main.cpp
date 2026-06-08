#ifndef __main_cpp__
#define __main_cpp__

#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>   // ACE_OS::sleep
#include <ace/Time_Value.h>

#include <nlohmann/json.hpp>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"

#include "data_store/client.hpp"
#include "data_store/service_gate.hpp"
#include "data_store/dep_watch.hpp"
#include "data_store/value.hpp"

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

#include "ds_config.hpp"
#include "rpi_serial.hpp"
#include "provisioning_policy.hpp"
#include "psk_gen.hpp"
#include "lua_config.hpp"


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
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l parsePeerOption: empty input\n")));
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

// ── Log ring buffer ────────────────────────────────────────────────
// Captures ACE log output → ds log.lwm2m.*.text for the cloud UI.
// Key may be overridden at startup for per-instance logs (bs / dm).
data_store::LogBuffer g_log("lwm2m", "log.lwm2m.text", "log.level.lwm2m");

// tinydtls log sink → ring buffer → ds → UI. tinydtls logs via its own
// dsrv_log() (fprintf), which bypasses ACE; this C-linkage callback routes
// each DTLS line into the same LogBuffer so the handshake shows in the UI.
extern "C" void dtls_set_log_sink(void (*)(int, const char*));
extern "C" void iot_dtls_log_sink(int /*level*/, const char* line) {
    std::string s(line ? line : "");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) g_log.append("dtls: " + s + "\n");
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
                              const std::string& endpoint,
                              iot::DsConfig*     ds = nullptr) {
    ::lwm2m::bootstrap::AccountProvisioning a;
    a.endpoint = endpoint;

    // ds-server overrides (when connected): iot.server.uri replaces the
    // DM Security instance's URI; iot.lifetime replaces the Server
    // instance's lifetime. iid 0 (Bootstrap) stays file-driven.
    std::optional<std::string>   dsUri;
    std::optional<std::uint32_t> dsLifetime;
    if (ds && ds->connected()) {
        dsUri      = ds->server_uri();
        dsLifetime = ds->lifetime();
    }

    using iot::lua_config::bool_or;
    using iot::lua_config::load_object_resources;
    using iot::lua_config::string_or;
    using iot::lua_config::uint_or;

    // Security Object instances: file naming mirrors today's "0.lua",
    // "1.lua" and the loaded order maps directly to the wire iid.
    for (std::uint16_t iid : {0, 1}) {
        auto m = load_object_resources(
            configDir + "/securityObject/" + std::to_string(iid) + ".lua");
        if (m.empty()) continue;

        ::lwm2m::bootstrap::SecurityInstance s;
        s.iid               = iid;
        s.serverUri         = string_or(m, 0, "");
        if (iid == 1 && dsUri) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l DM server URI from "
                                "data-store: %C\n"),
                       dsUri->c_str()));
            s.serverUri = *dsUri;
        }
        s.isBootstrapServer = bool_or(m, 1, iid == 0);
        s.securityMode      = static_cast<std::uint8_t>(uint_or(m, 2, 3));
        s.identity          = string_or(m, 3, "");
        s.secretKey         = string_or(m, 5, "");
        s.shortServerId     = static_cast<std::uint16_t>(uint_or(m, 10, iid + 1));
        a.security.push_back(std::move(s));
    }

    // Server Object instance 0 is the canonical DM-server account.
    auto srv = load_object_resources(configDir + "/serverObject/0.lua");
    if (!srv.empty()) {
        ::lwm2m::bootstrap::ServerInstance row;
        row.iid           = 0;
        row.shortServerId = static_cast<std::uint16_t>(uint_or(srv, 0, 1));
        row.lifetime      = uint_or(srv, 1, 86400);
        if (dsLifetime) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Server lifetime from "
                                "data-store: %u\n"),
                       static_cast<unsigned>(*dsLifetime)));
            row.lifetime  = *dsLifetime;
        }
        row.binding       = string_or(srv, 7, "U");
        a.server.push_back(std::move(row));
    }
    return a;
}

/// Attach BootstrapServer (on the Bootstrap port) + RegistrationServer +
/// canonical ObjectStore (for Discover output) on the DeviceMgmtServer
/// port. Install the 1 Hz tick to drive registry expiry.
struct ServerPlumbing {
    std::shared_ptr<::lwm2m::ClientRegistry>      registry;
    std::shared_ptr<::lwm2m::RegistrationServer>  regServer;
};

ServerPlumbing wire_server(std::shared_ptr<App>& app,
                           const std::string& configDir,
                           iot::DsConfig& ds) {
    auto registry = std::make_shared<::lwm2m::ClientRegistry>();
    auto bsServer = std::make_shared<::lwm2m::bootstrap::Server>();

    // Provision one account for the well-known test endpoint. Real
    // deployments would load multiple accounts from a registry file or
    // the DB; the wiring point stays the same.
    bsServer->add_account(
        load_provisioning_from_config(configDir, "urn:dev:client-1", &ds));

    // L9 / FUP-3: attach BOTH handlers to EVERY server-side service
    // context. processRequest's URI dispatch routes /bs → bsServer,
    // /rd → regServer regardless of which socket the datagram arrived
    // on. This lets a single-socket deployment (where BS and DM share
    // a port, or one of the binds failed with EADDRINUSE) still serve
    // both Bootstrap and Registration. A future multi-socket deploy
    // gets both handlers per socket for free.
    auto regServer = std::make_shared<::lwm2m::RegistrationServer>(registry);
    auto& services = app->udpAdapter()->services();
    for (auto& [type, ctx] : services) {
        ctx->coapAdapter()->bootstrapServer(bsServer);
        ctx->coapAdapter()->registrationServer(regServer);
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
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l expired %u registration(s)\n"),
                       static_cast<unsigned>(expired.size())));
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

    return ServerPlumbing{registry, regServer};
}

/// Build the client's ObjectStore + handlers and attach them to the
/// LwM2MClient ServiceContext. Returns the BootstrapClient + the
/// RegistrationClient so the caller can drive the FSM.
struct ClientPlumbing {
    std::shared_ptr<::lwm2m::ObjectStore>            store;
    std::shared_ptr<::lwm2m::DmClient>               dm;
    std::shared_ptr<::lwm2m::bootstrap::Client>      bs;
    std::shared_ptr<::lwm2m::RegistrationClient>     reg;

    /// FUP-DS-11 — DM server URI rebind mailbox shared between the
    /// data-store listener thread (writer) and the reactor tick
    /// (reader/consumer). Mutex-guarded because std::string + uint16_t
    /// aren't lock-free together.
    struct Rebind {
        std::mutex     mtx;
        bool           pending = false;
        std::string    host;
        std::uint16_t  port = 0;
    };
    std::shared_ptr<Rebind>                          rebind;
};

ClientPlumbing wire_client(std::shared_ptr<App>& app,
                           const std::string& endpoint,
                           const std::string& configDir,
                           const std::string& bsHost,
                           std::uint16_t bsPort,
                           iot::DsConfig& ds) {
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
    auto dsLifetime  = ds.lifetime();
    cfg.lifetime     = dsLifetime.value_or(86400);
    if (dsLifetime) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Registration lifetime from "
                            "data-store: %u\n"),
                   static_cast<unsigned>(cfg.lifetime)));
    }
    cfg.binding      = "U";
    cfg.lwm2mVersion = "1.1";
    plumb.reg = std::make_shared<::lwm2m::RegistrationClient>(cfg, *plumb.store);

    for (auto& [type, ctx] : services) {
        if (type != ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
        ctx->coapAdapter()->dmClient(plumb.dm);
        ctx->coapAdapter()->bootstrapClient(plumb.bs);
        // FUP-2 — feed inbound ACKs to the Registration FSM so the
        // 2.01 Created from Leshan transitions us from
        // AwaitingRegisterAck → Registered (which is what makes
        // should_send_update return true at the lifetime margin).
        ctx->coapAdapter()->registrationClient(plumb.reg);
    }

    // L9 stub 1 — Register POST after bootstrap commit. Runs on the
    // reactor thread (handle_bs_traffic invoked it); the tx itself
    // queues via send_async + notify so we don't recurse into the I/O
    // path inside the callback.
    std::weak_ptr<App>                          wapp_bs = app;
    std::weak_ptr<::lwm2m::RegistrationClient>  wreg_bs = plumb.reg;
    plumb.bs->on_done([wapp_bs, wreg_bs, &ds]
                      (const ::lwm2m::bootstrap::StagingBuffer& committed) {
        // Task F — persist the bootstrap-delivered DM credentials to the
        // data-store (write-only). The DM security instance is the
        // non-bootstrap PSK entry; its secretKey is raw bytes, so we hex-
        // encode to match how PSK keys are stored + consumed elsewhere.
        for (const auto& s : committed.security) {
            if (s.isBootstrapServer || s.securityMode != 0 /*PSK*/) continue;
            if (s.identity.empty() || s.secretKey.empty()) continue;
            const std::string key_hex = iot::hex_encode(
                reinterpret_cast<const unsigned char*>(s.secretKey.data()),
                s.secretKey.size());
            if (ds.set_dm_credentials(s.identity, key_hex)) {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l DM credentials "
                                    "persisted to data-store (identity=%C)\n"),
                           s.identity.c_str()));
            }
            break;
        }
        auto a = wapp_bs.lock();
        auto r = wreg_bs.lock();
        if (!a || !r) return;
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l bootstrap commit done; "
                            "sending Register\n")));
        auto payload = r->build_register_request(next_msgid(),
                                                 std::string{static_cast<char>(0x01)});
        tx_via(*a, payload, ::UDPAdapter::ServiceType_t::LwM2MClient);
    });

    // FUP-DS-11 — DM server URI rebind mailbox. Listener thread fills
    // it from a Key::ServerUri change; reactor tick consumes it once
    // state is cleanly Unregistered (so the Deregister to the old
    // server has been ack'd), swaps the LwM2MClient peer, then the
    // existing Unregistered branch sends Register to the new peer.
    auto rebind = std::make_shared<ClientPlumbing::Rebind>();
    plumb.rebind = rebind;

    // 1 Hz ticker: drives Update emission + Observe pmax + (TODO) initial
    // Register once bootstrap completes.
    std::weak_ptr<::lwm2m::RegistrationClient> wreg = plumb.reg;
    std::weak_ptr<::lwm2m::DmClient>           wdm  = plumb.dm;
    std::weak_ptr<App>                         wapp = app;
    std::weak_ptr<::lwm2m::bootstrap::Client>  wbs  = plumb.bs;
    // Task K — cooldown so a persistently-rejecting DM doesn't spin the
    // bootstrap. Shared across ticks; default-constructed = epoch.
    auto reboot_after =
        std::make_shared<std::chrono::steady_clock::time_point>();
    app->udpAdapter()->on_tick_client([wreg, wdm, wapp, rebind, wbs,
                                       reboot_after]() {
        auto reg = wreg.lock();
        auto dm  = wdm.lock();
        auto a   = wapp.lock();
        if (!reg || !dm || !a) return;

        const auto svc = ::UDPAdapter::ServiceType_t::LwM2MClient;
        const auto now = std::chrono::steady_clock::now();

        // FUP-DS-11 — if a rebind is queued and we're between
        // registrations, swap the LwM2MClient peer FIRST so the next
        // Register goes to the new server. Must run before the
        // Unregistered branch below.
        if (reg->state() == ::lwm2m::RegistrationState::Unregistered) {
            std::lock_guard<std::mutex> g(rebind->mtx);
            if (rebind->pending) {
                for (auto& [type, ctx] : a->udpAdapter()->services()) {
                    if (type != ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
                    ctx->peerHost(rebind->host);
                    ctx->peerPort(rebind->port);
                }
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l swapped DM peer to "
                                    "%C:%u for next Register\n"),
                           rebind->host.c_str(),
                           static_cast<unsigned>(rebind->port)));
                rebind->pending = false;
            }
        }

        // L9 fallback (Leshan interop) — if we never went through
        // Bootstrap (e.g. the server is a plain LwM2M DM server with no
        // BS account for us), fire the initial Register on the first
        // tick after startup. Also the second leg of FUP-DS-10/11:
        // after a triggered Deregister completes, state returns to
        // Unregistered and this branch sends a fresh Register with
        // the new endpoint (via RegistrationClient::endpoint()) and/or
        // to the freshly-swapped peer above.
        if (reg->state() == ::lwm2m::RegistrationState::Unregistered &&
            !reg->is_disabled()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l no bootstrap; "
                                "sending Register directly\n")));
            auto payload = reg->build_register_request(
                next_msgid(),
                std::string{static_cast<char>(0x10)});
            tx_via(*a, payload, svc);
            // Endpoint change is naturally satisfied by this Register;
            // clear the flag so we don't double-act on it later.
            reg->clear_pending_reregister();
        }

        // FUP-DS-10 — endpoint hot-reload kicks the re-register cycle.
        // Only act when we're cleanly Registered; states in flight
        // (AwaitingRegisterAck / AwaitingUpdateAck / AwaitingDeregisterAck)
        // leave the flag set, picked up on a later tick.
        if (reg->pending_reregister() &&
            reg->state() == ::lwm2m::RegistrationState::Registered) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l endpoint changed — "
                                "sending Deregister, will rejoin under new "
                                "endpoint on next tick\n")));
            auto payload = reg->build_deregister_request(
                next_msgid(),
                std::string{static_cast<char>(0x20)});
            tx_via(*a, payload, svc);
            reg->clear_pending_reregister();
        }

        // L9 stub 2 — Update POST when the lifetime margin elapses.
        if (reg->should_send_update(now)) {
            auto payload = reg->build_update_request(
                next_msgid(),
                std::string{static_cast<char>(0x02)},
                /*withAdvertisedSet*/ false);
            tx_via(*a, payload, svc);
            reg->note_update_sent(now);
        }

        // Task K — DM rejected us (registration FSM in Failed after a
        // 4.0x). Fall back to the bootstrap server for fresh DM
        // credentials. `should_rebootstrap` centralises the trigger
        // (here: registration rejection; a DM DTLS auth failure surfaces
        // the same way once the handshake gives up). Cooldown prevents a
        // spin against a server that keeps rejecting. NOTE: this re-POSTs
        // /bs over the LwM2MClient service, correct when BS+DM share a
        // peer; the separate-DM-peer topology needs a peer swap back to
        // BS first (integration P2 / FUP-DS-11 rebind).
        if (reg->state() == ::lwm2m::RegistrationState::Failed &&
            iot::should_rebootstrap(/*dm_dtls_failed*/false,
                                    /*dm_registration_rejected*/true) &&
            now >= *reboot_after) {
            if (auto bs = wbs.lock()) {
                ACE_ERROR((LM_WARNING,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l DM rejected "
                                    "registration — re-bootstrapping for fresh "
                                    "DM credentials\n")));
                auto payload = bs->build_bs_request(
                    next_msgid(), std::string{static_cast<char>(0x30)});
                tx_via(*a, payload, svc);
                *reboot_after = now + std::chrono::seconds(30);
            }
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
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l arg %C=%C\n"),
                       ent.first.c_str(), ent.second.c_str()));
        }
    }

    // Device default is client (it runs client-only and may be launched with
    // no args, fully ds-driven). The cloud passes role=server explicitly for
    // the BS/DM instances. Only an explicit, invalid role is an error.
    UDPAdapter::Role_t role = UDPAdapter::Role_t::CLIENT;
    if(argValueMap["role"] == "server") {
        role = UDPAdapter::Role_t::SERVER;
    } else if(!argValueMap["role"].empty() && argValueMap["role"] != "client") {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D lwm2m:thread:%t %M %N:%l invalid role '%C' (use client|server)\n"),
                          argValueMap["role"].c_str()),
                         -1);
    }

    UDPAdapter::Scheme_t scheme;
    // Bootstrap host/port are resolved AFTER the ds connection below: for a
    // client they come from iot.bs.uri (commissioned via device-ui), falling
    // back to a bs= CLI arg.
    std::string bsHost;
    std::uint16_t bsPort = 0;

    std::string selfHost;
    std::uint16_t selfPort;
    // Local bind defaults to coaps://0.0.0.0:5684 (DTLS) for a ds-driven
    // launch with no args; the scheme is taken from here.
    const std::string localUri = !argValueMap["local"].empty()
                                     ? argValueMap["local"]
                                     : std::string("coaps://0.0.0.0:5684");
    parsePeerOption(localUri, scheme, selfHost, selfPort);

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l scheme=%d host=%C port=%u\n"),
               static_cast<int>(scheme), selfHost.c_str(),
               static_cast<unsigned>(selfPort)));

    // ── PSK provisioning (tasks A/E) ──────────────────────────────────
    // Connect the data-store config plane early so the serial-derived
    // endpoint + BS DTLS credentials are available before we wire the
    // DTLS adapter. ds-server is optional (absence → CLI/compiled-in
    // fallback). On a Raspberry Pi the serial is auto-filled here.
    iot::DsConfig ds(argValueMap["ds-sock"]);

    // Wire logging BEFORE the provisioning park below so the "awaiting
    // provisioning" notice + any startup failures reach the UI (ds), not just
    // stderr. start()/open() are idempotent — the later calls (after the
    // cloud-instance key setup) just refine the level key.
    g_log.start();
    dtls_set_log_sink(&iot_dtls_log_sink);   // DTLS (tinydtls) logs → UI
    if (auto* cli = ds.client()) { g_log.apply_level(*cli); g_log.open(*cli, 5, 1); }

    // Bootstrap URI + BS PSK (client only): ds-driven from iot.bs.uri /
    // iot.bs.psk.* (commissioned via device-ui), with bs=/identity=/secret=
    // CLI fallback. PARK here until commissioned rather than exiting — an
    // unprovisioned device stays alive (no crash-loop that resets the log
    // buffer + blocks commissioning) and resumes as soon as the ds watch sees
    // the values appear. Running as `engineer` is what lets the watch read
    // the gid:engineer PSK keys.
    if (UDPAdapter::Role_t::CLIENT == role) {
        std::string bsUri;
        bool logged_wait = false;
        for (;;) {
            bsUri = ds.bs_uri().value_or(argValueMap["bs"]);
            // BS PSK identity is DERIVED from the endpoint (sha256), so only
            // the secret (iot.bs.psk.key) is commissioned — not the identity.
            auto pkey = ds.bs_psk_key();
            const bool have_psk =
                (pkey && !pkey->empty()) ||
                (!argValueMap["identity"].empty() && !argValueMap["secret"].empty());
            if (!bsUri.empty() && have_psk) break;
            if (!logged_wait) {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l awaiting provisioning "
                                    "(iot.bs.uri + BS PSK via device-ui commissioning)\n")));
                logged_wait = true;
            }
            ACE_OS::sleep(ACE_Time_Value(5, 0));
        }
        UDPAdapter::Scheme_t bsScheme;
        parsePeerOption(bsUri, bsScheme, bsHost, bsPort);
    }

    const bool rpi = iot::is_rpi();
    iot::EndpointResolution epres = iot::resolve_endpoint(
        argValueMap["ep"], ds.serial(), rpi,
        rpi ? iot::read_rpi_serial() : std::string());
    if (!epres.serial_to_write.empty()) {
        if (ds.set_serial(epres.serial_to_write)) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l RPi serial auto-filled "
                                "to data-store: %C\n"),
                       epres.serial_to_write.c_str()));
        }
    }

    std::string identity("97554878B284CE3B727D8DD06E87659A"), secret("3894beedaa7fe0eae6597dc350a59525");
    if(scheme == UDPAdapter::Scheme_t::CoAPs) {
        auto dsKey = ds.bs_psk_key();
        if (UDPAdapter::Role_t::CLIENT == role) {
            // BS DTLS PSK: identity is DERIVED from the endpoint — both the
            // device and the cloud BS compute identity = sha256(endpoint), so
            // it is never stored/commissioned. Only the secret (iot.bs.psk.key)
            // is commissioned, with secret= CLI as a dev fallback.
            identity = iot::sha256_hex(endpoint);
            if (dsKey && !dsKey->empty())            secret = *dsKey;
            else if (!argValueMap["secret"].empty()) secret = argValueMap["secret"];
            else {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK secret missing "
                                    "(provision iot.bs.psk.key or pass secret=)\n")));
                return(-2);
            }
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK identity=sha256(endpoint)=%C\n"),
                       identity.c_str()));
        } else if (!argValueMap["identity"].empty() &&
                   !argValueMap["secret"].empty()) {
            identity.assign(argValueMap["identity"]);
            secret.assign(argValueMap["secret"]);
        } else {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l identity or secret missing "
                                "for coaps (no CLI args and no data-store BS PSK)\n")));
            return(-2);
        }
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

    // Server role: (re)register per-device BS/DM PSKs from the cloud's
    // cloud.endpoint.credentials array. BS identity = sha256(serial) — exactly
    // what the device derives from its endpoint — and DM identity = dm.psk.id.
    // add_credential() is a keyed store, so this is additive + idempotent.
    // Reading the array requires gid:cloud-svc (the BS runs as cloud-svc).
    auto register_endpoint_creds = [&](auto* dtls) {
        if (!dtls || UDPAdapter::Role_t::SERVER != role || !ds.client()) return;
        const bool is_bs = argValueMap["lwm2m-instance"] == "bs";
        std::vector<data_store::Client::GetResult> got;
        auto rs = ds.client()->get({std::string("cloud.endpoint.credentials")}, got);
        if (!rs.ok || got.empty() || !got[0].has_value) return;
        auto s = data_store::to_string(got[0].value);
        if (!s || s->empty()) return;
        try {
            auto arr = nlohmann::json::parse(*s);
            if (!arr.is_array()) return;
            int n = 0;
            for (auto& e : arr) {
                if (!e.is_object()) continue;
                std::string ident, key;
                if (is_bs) {
                    const std::string serial = e.value("serial", std::string());
                    key = e.value("bs.psk.key", std::string());
                    if (!serial.empty()) ident = iot::sha256_hex(serial);
                } else {
                    ident = e.value("dm.psk.id", std::string());
                    key   = e.value("dm.psk.key", std::string());
                }
                if (!ident.empty() && !key.empty()) { dtls->add_credential(ident, key); ++n; }
            }
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l registered %d per-endpoint %C PSK(s)\n"),
                       n, is_bs ? "BS" : "DM"));
        } catch (const std::exception& ex) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l cloud.endpoint.credentials parse: %C\n"),
                       ex.what()));
        }
    };

    if(UDPAdapter::Scheme_t::CoAPs == scheme) {
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            ent.second->dtlsAdapter()->add_credential(identity, secret);
            register_endpoint_creds(ent.second->dtlsAdapter());
        }
    }

    // L9 wiring: instantiate the LwM2M handlers + ObjectStore and bind
    // them to the relevant ServiceContext_t CoAPAdapters. configDir
    // defaults to the deployed /etc/iot/config so a ds-driven launch with no
    // args works; dev builds running from apps/build/ pass config=../config.
    const std::string configDir = !argValueMap["config"].empty()
                                      ? argValueMap["config"]
                                      : std::string("/etc/iot/config");
    // Endpoint resolved above (task E) from CLI ep= / data-store serial /
    // RPi auto-detect. When non-RPi and nothing is provisioned yet,
    // epres.ready is false → fall back to the legacy placeholder so the
    // binary still comes up; registration effectively waits for the
    // installer to enter a serial via device-ui (which re-fires the watch).
    std::string endpoint = epres.ready ? epres.endpoint
                                       : std::string("urn:dev:client-1");

    // ds-server config-plane: `ds` connected above. `iot.endpoint` /
    // `iot.lifetime` / `iot.server.uri` continue to override file
    // defaults via the watch + on_change handler below.
    g_log.start();  // register ACE callback now that ACE is initialised

    // L16/D5 — services.lwm2m.{client,server}.enable gate. Minimal
    // cut: publish state at startup, park if disabled, return when
    // re-enabled (which the operator will follow with a daemon
    // restart for the registration to take effect). Full
    // mid-session Deregister + drop-observations machinery is FUP
    // (this is the L16 plan §D5 "split into D5a/D5b if it grows
    // past one PR" — we ship D5a here).
    // L17a/D4 — dependency watch alongside the svc gate. Both
    // lwm2m.client and lwm2m.server depend on net.router for
    // forwarding. When net.router is disabled, the lwm2m daemon
    // parks via the same set_disabled path the svc gate uses.
    // Cloud LwM2M server instance — bs or dm. When set, the binary
    // self-reports state to services.cloud.lwm2m.<instance>.state
    // instead of the device-side services.lwm2m.server.* keys.
    const std::string lwm2m_instance = argValueMap["lwm2m-instance"];

    // Per-instance log key for cloud server role
    if (!lwm2m_instance.empty()) {
        g_log.set_log_key("log.lwm2m." + lwm2m_instance + ".text");
        g_log.set_level_key("log.level.lwm2m." + lwm2m_instance);
    }
    // Seed schema keys so ds-server knows about them
    if (auto* cli = ds.client()) {
        cli->set("log.lwm2m.text",
                 data_store::Value{std::string("")}, 100);
        cli->set("log.lwm2m.bs.text",
                 data_store::Value{std::string("")}, 100);
        cli->set("log.lwm2m.dm.text",
                 data_store::Value{std::string("")}, 100);
    }

    if (auto* cli = ds.client()) {
        g_log.apply_level(*cli);
        g_log.flush(*cli);  // push startup logs immediately
    }

    // ── Resource telemetry (L22) ──────────────────────────────────
    // Publish this container's CPU/mem/fd/threads every 10s. The lwm2m
    // binary's udpAdapter owns the singleton reactor on its OWN thread, so a
    // timer scheduled here wouldn't dispatch — StatsPublisher uses its own
    // private reactor + thread (run_reactor_thread=true, the default).
    // Prefix tracks role: cloud bs/dm instance, else device client/server.
    const std::string stats_prefix =
        !lwm2m_instance.empty()
            ? ("services.cloud.lwm2m." + lwm2m_instance)
            : (UDPAdapter::Role_t::CLIENT == role ? "services.lwm2m.client"
                                                  : "services.lwm2m.server");
    data_store::StatsPublisher g_stats(
        stats_prefix,
        [&ds](const std::vector<data_store::KV>& kv) {
            if (auto* c = ds.client()) c->set(kv);
        });
    if (auto* cli = ds.client()) {
        if (g_stats.open() != 0) {   // run_reactor_thread=true (own reactor)
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l stats publisher "
                                "open failed\n")));
        }
        (void)cli;
    }


    // Flush the log ring-buffer every 5s via LogBuffer's own ACE reactor
    // timer (private reactor on its own thread — same pattern as
    // StatsPublisher). min_bytes=1 so even a low-traffic daemon's lines
    // actually reach ds (200 suppressed small buffers → empty Logs page).
    if (auto* cli = ds.client()) g_log.open(*cli, 5, 1);

    std::unique_ptr<data_store::ServiceGate> svc_gate;
    std::unique_ptr<data_store::DepWatch>    dep_watch;
    if (auto* cli = ds.client()) {
        // Cloud server mode — self-report running state immediately
        // after the ds connection is established.
        if (!lwm2m_instance.empty()) {
            const std::string sk = std::string("services.cloud.lwm2m.")
                                   + lwm2m_instance + ".state";
            auto rs = cli->set(sk, data_store::Value{std::string("running")});
            if (!rs.ok) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l set %C=running"
                                    " failed: %C\n"),
                           sk.c_str(), rs.err.c_str()));
            }
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l cloud lwm2m-%C "
                                "self-reported running\n"),
                       lwm2m_instance.c_str()));
        }

        const std::string svc_name =
            (UDPAdapter::CLIENT == role) ? "lwm2m.client" : "lwm2m.server";
        svc_gate = std::make_unique<data_store::ServiceGate>(*cli, svc_name);
        dep_watch = std::make_unique<data_store::DepWatch>(
            *cli, std::vector<std::string>{"net.router"});

        // L17a/D4 — dep check before svc check (dep_down > disabled).
        if (!dep_watch->healthy()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l dep %C unhealthy "
                                "at startup; parking\n"),
                       dep_watch->unhealthy_dep().c_str()));
            svc_gate->publish_state("disabled");
            dep_watch->wait();
        }

        if (!svc_gate->enabled()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l services.%C.enable=false "
                                "at startup; parking until re-enabled\n"),
                       svc_name.c_str()));
            svc_gate->publish_state("disabled");
            auto v = svc_gate->wait();
            if (!v.has_value()) return 0;   // shutdown
        }
        svc_gate->publish_state("running");
    }

    ClientPlumbing clientPlumbing;
    ServerPlumbing serverPlumbing;
    if (UDPAdapter::CLIENT == role) {
        clientPlumbing = wire_client(app, endpoint, configDir, bsHost, bsPort, ds);
    } else {
        serverPlumbing = wire_server(app, configDir, ds);
    }

    // L16/D5b — watcher thread for mid-session services.lwm2m.*.enable
    // transitions. On disable:
    //   client: reg->set_disabled(true) — reactor tick sends
    //           Deregister (via the auto-set pending_reregister) and
    //           then parks the FSM in Unregistered (the is_disabled
    //           branch skips auto-Register).
    //   server: regServer->set_disabled(true) — handle() rejects new
    //           Register with 5.03; we drop the active-registration
    //           map so currently-registered clients see NotFound on
    //           their next Update and lifetime-expire on their own.
    // On re-enable: clear the flag; the reactor tick (client) or
    // handle() (server) starts accepting again.
    std::atomic<bool> svc_stop{false};
    std::thread svc_watcher_thread;
    std::thread dep_watcher_thread;
    if (svc_gate) {
        auto* reg       = clientPlumbing.reg.get();
        auto  regServer = serverPlumbing.regServer;
        auto  registry  = serverPlumbing.registry;
        svc_watcher_thread = std::thread(
            [&svc_stop, gate = svc_gate.get(), reg, regServer, registry] {
                while (!svc_stop.load(std::memory_order_acquire)) {
                    auto v = gate->wait();
                    if (!v.has_value()) return;            // shutdown
                    if (*v) {
                        // false -> true (re-enable)
                        if (reg) reg->set_disabled(false);
                        if (regServer) regServer->set_disabled(false);
                        gate->publish_state("running");
                    } else {
                        // true -> false (disable)
                        gate->publish_state("stopping");
                        if (reg) reg->set_disabled(true);
                        if (regServer) {
                            regServer->set_disabled(true);
                            if (registry) registry->load_from({});
                        }
                        gate->publish_state("disabled");
                    }
                }
            });

        // L17a/D4 — dep watcher thread. On dep transition:
        //   dep unhealthy → set_disabled (same as svc disable)
        //   dep recovered  → clear_disabled (re-enable, if svc also
        //                     says enabled — checked via is_disabled()).
        dep_watcher_thread = std::thread(
            [&svc_stop, dw = dep_watch.get(),
             gate = svc_gate.get(), reg, regServer, registry] {
                while (!svc_stop.load(std::memory_order_acquire)) {
                    if (!dw->wait()) return;               // shutdown
                    if (!dw->healthy()) {
                        // dep went unhealthy → publish + disable
                        gate->publish_state("stopping");
                        if (reg) reg->set_disabled(true);
                        if (regServer) {
                            regServer->set_disabled(true);
                            if (registry) registry->load_from({});
                        }
                        gate->publish_state("disabled");
                    } else {
                        // deps recovered → re-enable
                        if (reg) reg->set_disabled(false);
                        if (regServer) regServer->set_disabled(false);
                        gate->publish_state("running");
                    }
                }
            });
    }

    // FUP-DS-9 / FUP-DS-10 / FUP-DS-11 — apply hot-reloaded values to
    // the live LwM2M FSM.
    //   * iot.lifetime   — lock-free atomic, next Update tick uses it
    //   * iot.endpoint   — flips pending_reregister, reactor tick fires
    //                      Deregister → Unregistered → Register cycle
    //   * iot.server.uri — fills the rebind mailbox + flips
    //                      pending_reregister; reactor tick swaps the
    //                      DM peer after Unregistered, then Register
    //                      goes to the new server. Plain CoAP only;
    //                      CoAPs requires DTLS teardown + reconnect
    //                      tracked separately if/when needed.
    if (clientPlumbing.reg) {
        std::weak_ptr<::lwm2m::RegistrationClient>      wreg = clientPlumbing.reg;
        std::shared_ptr<ClientPlumbing::Rebind>         rebind = clientPlumbing.rebind;
        ds.on_change([wreg, rebind, &ds, started_bs_psk = secret]
                     (iot::DsConfig::Key k) {
            // Task G — a BS PSK change underneath us (e.g. an engineer
            // edits iot.bs.psk.key via ds-cli in dev-mode) means the next
            // DTLS handshake needs the new key. Self-exit; systemd
            // (Restart=always) relaunches us cleanly with fresh
            // credentials. The client never writes iot.bs.psk.key itself,
            // so any change here is genuinely external.
            if (k == iot::DsConfig::Key::BsPskKey) {
                const std::string cur = ds.bs_psk_key().value_or("");
                if (iot::should_restart_on_psk_change(/*initialized*/true,
                                                      started_bs_psk, cur)) {
                    ACE_ERROR((LM_WARNING,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK changed "
                                        "— exiting for systemd restart with new "
                                        "credentials\n")));
                    ::exit(0);
                }
                return;
            }
            auto reg = wreg.lock();
            if (!reg) return;
            if (k == iot::DsConfig::Key::Lifetime) {
                auto v = ds.lifetime();
                if (!v) return;
                reg->set_lifetime(*v);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l applied "
                                    "iot.lifetime=%u to live "
                                    "RegistrationClient — next Update tick "
                                    "uses it\n"),
                           static_cast<unsigned>(*v)));
            } else if (k == iot::DsConfig::Key::Endpoint) {
                auto v = ds.endpoint();
                if (!v) return;
                reg->set_endpoint(*v);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l applied "
                                    "iot.endpoint=%C — re-register cycle "
                                    "queued for next 1Hz tick\n"),
                           v->c_str()));
            } else if (k == iot::DsConfig::Key::ServerUri && rebind) {
                auto v = ds.server_uri();
                if (!v) return;
                UDPAdapter::Scheme_t sc{};
                std::string h;
                std::uint16_t p = 0;
                parsePeerOption(*v, sc, h, p);
                if (h.empty() || p == 0) {
                    ACE_ERROR((LM_WARNING,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l iot.server.uri "
                                        "'%C' failed to parse — rebind skipped\n"),
                               v->c_str()));
                    return;
                }
                if (sc == UDPAdapter::Scheme_t::CoAPs) {
                    ACE_ERROR((LM_WARNING,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l iot.server.uri "
                                        "scheme=coaps live rebind not yet "
                                        "supported (needs DTLS teardown); "
                                        "applying peer change only — next "
                                        "Register will still use stale DTLS "
                                        "session\n")));
                }
                {
                    std::lock_guard<std::mutex> g(rebind->mtx);
                    rebind->pending = true;
                    rebind->host    = h;
                    rebind->port    = p;
                }
                reg->request_reregister();
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l applied "
                                    "iot.server.uri=%C — rebind to %C:%u "
                                    "queued for next 1Hz tick (after "
                                    "Deregister to old server completes)\n"),
                           v->c_str(), h.c_str(),
                           static_cast<unsigned>(p)));
            }
        });
    }

    // UDP socket churn (peer torn down between writes) would otherwise
    // raise SIGPIPE and kill the process; ignore it. SIGINT/SIGTERM are
    // handled by the ACE reactor loop inside UDPAdapter::start().
    ::signal(SIGPIPE, SIG_IGN);

    // Readline is wired for both roles. Client-side the CLI drives
    // outbound CoAP toward the configured server; server-side the
    // same registry is available for hand-crafted /push / /set frames
    // via the low-level `post` command (LwM2M-level commands like
    // `read`/`write`/`observe` require an LwM2MClient ServiceContext
    // and will emit a clear error otherwise).
    if(UDPAdapter::CLIENT == role) {
        // App::start activates the reactor on its own ACE_Task thread
        // so the main thread can drive readline.
        app->start(role, scheme);
    } else {
        // Server: reactor runs on its own thread via App::start so the
        // main thread can host the REPL just like the client path.
        app->start(role, scheme);
    }

    // Readline requires a TTY on stdin. In container/CI runs stdin is
    // typically /dev/null (`-d` detached mode), so reading would hit
    // EOF immediately and the process would exit before any traffic
    // completes. Detect that and just park the main thread on the
    // reactor task instead.
    if (::isatty(STDIN_FILENO)) {
        Readline rline(app);
        rline.init();
        rline.start();
    } else {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l stdin is not a TTY; "
                            "running in non-interactive mode (reactor only)\n")));
        // ACE_Task::wait() blocks until svc() returns.
        app->udpAdapter()->wait();
    }
    app->stop();

    // Cloud LwM2M server mode — self-report exited at shutdown.
    if (!lwm2m_instance.empty()) {
        if (auto* cli = ds.client()) {
            const std::string sk = std::string("services.cloud.lwm2m.")
                                   + lwm2m_instance + ".state";
            cli->set(sk, data_store::Value{std::string("exited")});
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l cloud lwm2m-%C "
                                "self-reported exited\n"),
                       lwm2m_instance.c_str()));
        }
    }

    // Stop the log-flush timer + final flush (ds still alive).
    g_log.close();

    // L16/D5b cleanup — wake the watcher thread + join.
    svc_stop.store(true, std::memory_order_release);
    if (svc_gate) svc_gate->shutdown();
    if (dep_watch) dep_watch->shutdown();
    if (svc_watcher_thread.joinable()) svc_watcher_thread.join();
    if (dep_watcher_thread.joinable()) dep_watcher_thread.join();

    return(0);
}





#endif /* __main_cpp__*/