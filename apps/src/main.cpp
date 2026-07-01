#ifndef __main_cpp__
#define __main_cpp__

#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cerrno>    // errno (OTA stage-request write)
#include <cstdint>
#include <cstdio>    // std::rename/std::remove (OTA stage-request handoff)
#include <cstdlib>   // std::system (A/B bank-switch launcher)
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

// systemd software watchdog: speak the sd_notify "WATCHDOG=1" wire protocol
// directly so we don't pull in libsystemd. Raw-socket glue mirrors
// dtls_adapter.cpp, which likewise drops to ::sendto for the tinydtls path.
#include <cstddef>      // offsetof
#include <cstring>      // std::memset / memcpy / strlen
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>     // close

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
#include "lwm2m_object_sensors.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_object_stubs.hpp"
#include "lwm2m_registration.hpp"
#include "lwm2m_registration_client.hpp"
#include "lwm2m_registration_server.hpp"
#include "lwm2m_send_server.hpp"
#include "lwm2m_send_uploader.hpp"            // v2 Send telemetry (TDD §3b #1)
#include "lwm2m_durable_sample_buffer.hpp"    // make_sample_buffer / DurableSampleBuffer

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

// Zero-touch bootstrap (apps/docs/tdd-bs-hkdf-zerotouch.md): the Key-Encryption
// -Key that unwraps cloud.bs.master.key. Delivered OUT-OF-BAND, never via the
// data-store: a systemd credential ($CREDENTIALS_DIRECTORY/bs_kek, the
// LoadCredential path), an explicit file (IOT_BS_MASTER_KEK_FILE), or the hex
// value inline (IOT_BS_MASTER_KEK). Returns the KEK as hex, or "" when none is
// configured → the HKDF tier stays disabled (the commissioned per-device tier
// still serves). Read once at server wiring and captured by the resolvers.
static std::string load_bs_master_kek_hex() {
    auto read_first_line = [](const std::string& path) -> std::string {
        std::ifstream f(path);
        std::string line;
        if (f.is_open()) std::getline(f, line);
        // trim trailing CR/space/newline
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r' ||
                line.back() == ' '  || line.back() == '\t'))
            line.pop_back();
        return line;
    };
    if (const char* inl = std::getenv("IOT_BS_MASTER_KEK"); inl && *inl)
        return std::string(inl);
    if (const char* fp = std::getenv("IOT_BS_MASTER_KEK_FILE"); fp && *fp)
        return read_first_line(fp);
    if (const char* cd = std::getenv("CREDENTIALS_DIRECTORY"); cd && *cd)
        return read_first_line(std::string(cd) + "/bs_kek");
    return std::string();
}

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

namespace {
// ── v2 Send telemetry (TDD §3b #1) — numeric Object-33000 vehicle signals,
// mapped ds key ↔ resource id (under base "/33000/0/"). link (/10) + dtc (/8)
// are non-numeric, so they are not part of a SenML telemetry batch.
struct VehSig { const char* key; const char* rid; };
constexpr VehSig kVehSigs[] = {
    {"vehicle.speed", "0"}, {"vehicle.rpm", "1"},   {"vehicle.coolant", "2"},
    {"vehicle.throttle", "3"}, {"vehicle.load", "4"}, {"vehicle.fuel", "5"},
    {"vehicle.iat", "6"},   {"vehicle.maf", "7"},
};
// Read the numeric vehicle.* signals from ds into a timestamped Sample.
// Returns false when none are present/numeric (nothing to offer this tick).
bool build_vehicle_sample(data_store::Client* cli, double now_unix,
                          ::lwm2m::telemetry::Sample& out) {
    if (!cli) return false;
    std::vector<std::string> keys;
    for (const auto& s : kVehSigs) keys.emplace_back(s.key);
    std::vector<data_store::Client::GetResult> got;
    if (!cli->get(keys, got).ok || got.size() != keys.size()) return false;
    out = ::lwm2m::telemetry::Sample{};
    out.timeUnix = now_unix;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!got[i].has_value) continue;
        auto s = data_store::to_string(got[i].value);
        if (!s || s->empty()) continue;
        try { out.values.emplace_back(kVehSigs[i].rid, std::stod(*s)); }
        catch (...) { /* non-numeric → skip this signal */ }
    }
    return !out.values.empty();
}
} // namespace

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

/// Attach BootstrapServer (on the Bootstrap port) + RegistrationServer +
/// canonical ObjectStore (for Discover output) on the DeviceMgmtServer
/// port. Install the 1 Hz tick to drive registry expiry.
struct ServerPlumbing {
    std::shared_ptr<::lwm2m::ClientRegistry>      registry;
    std::shared_ptr<::lwm2m::RegistrationServer>  regServer;
};

ServerPlumbing wire_server(std::shared_ptr<App>& app,
                           iot::DsConfig& ds,
                           const std::string& lwm2mInstance) {
    auto registry = std::make_shared<::lwm2m::ClientRegistry>();
    auto bsServer = std::make_shared<::lwm2m::bootstrap::Server>();

    // Bootstrap provisioning is 100% data-store driven — there is no static
    // config account. The cloud BS resolver below synthesises the BS+DM
    // accounts per /bs from cloud.endpoint.credentials + cloud.{bs,dm}.*.

    // Cloud Bootstrap server (lwm2m-instance=bs): synthesise the DM account
    // per /bs from the live data-store instead of a static config file.
    //
    // The DM PSK is minted ONCE by iot-cloudd at provisioning time and
    // stored in cloud.endpoint.credentials (keyed by serial / formatted
    // identity); lwm2m-dm loads those same creds at startup and validates
    // the client's DTLS handshake against them. So the BS must hand the
    // client the SAME credential — not a freshly-generated one — otherwise
    // the post-bootstrap DTLS to the DM would fail. We look the record up
    // by endpoint and build two Security instances + the DM Server object:
    //   Security/0 (BS): RID0=cloud.bs.uri, RID1=true, RID2=0 (PSK),
    //                    RID3=sha256(ep)[:32], RID5=bs.psk.key, RID10=0 (ignored)
    //   Security/1 (DM): RID0=cloud.dm.uri, RID1=false, RID2=0 (PSK),
    //                    RID3=dm.psk.id, RID5=dm.psk.key (hex), RID10=1
    //   Server/0:        SSID=1, lifetime=cloud.dm.lifetime, binding=cloud.dm.binding
    // The /rd registration location is assigned by the DM at Register and
    // used by the client for periodic Update — it is not minted here.
    if (lwm2mInstance == "bs") {
        const std::string bsKekHex = load_bs_master_kek_hex();
        bsServer->provisioning_resolver(
            [&ds, bsKekHex](const std::string& ep)
                -> std::optional<::lwm2m::bootstrap::AccountProvisioning> {
            auto* cli = ds.client();
            if (!cli) return std::nullopt;

            std::vector<data_store::Client::GetResult> got;
            auto rs = cli->get({std::string("cloud.endpoint.credentials"),
                                std::string("cloud.dm.uri"),
                                std::string("cloud.dm.lifetime"),
                                std::string("cloud.dm.binding"),
                                std::string("cloud.bs.uri"),
                                std::string("cloud.bs.master.key"),
                                std::string("cloud.tenants")}, got);
            if (!rs.ok || got.size() < 5) return std::nullopt;

            auto str_at = [&](std::size_t i) -> std::string {
                return (i < got.size() && got[i].has_value)
                           ? data_store::to_string(got[i].value).value_or("")
                           : std::string();
            };
            const std::string credsJson = str_at(0);
            const std::string dmUri     = str_at(1);
            // cloud.dm.lifetime is INTEGER-typed in the schema (default 90, the
            // NAT-keepalive value). Read it as an int — to_string() returns
            // nullopt on an integer Value, which used to silently fall through to
            // the old 86400 default so the device always registered with a 24h
            // lifetime regardless of cloud.dm.lifetime. Tolerate a string-typed
            // value too (legacy). Default 90 to match the schema, not 86400.
            std::uint32_t lifetime = 90;
            if (got.size() > 2 && got[2].has_value) {
                if (auto n = data_store::to_uint32(got[2].value)) {
                    lifetime = *n;
                } else if (auto i = data_store::to_int32(got[2].value)) {
                    if (*i > 0) lifetime = static_cast<std::uint32_t>(*i);
                } else if (auto s = data_store::to_string(got[2].value); s && !s->empty()) {
                    try { lifetime = static_cast<std::uint32_t>(std::stoul(*s)); }
                    catch (...) {}
                }
            }
            std::string binding = str_at(3);
            if (binding.empty()) binding = "U";
            const std::string bsUri = str_at(4);

            // Unwrap the HKDF master (zero-touch tier) if a KEK is configured.
            // str_at(5) is the AES-256-GCM-wrapped cloud.bs.master.key blob; a
            // missing KEK or a bad/tampered blob → "" → tier disabled, fail
            // closed to the commissioned per-device lookup below.
            const std::string masterHex =
                bsKekHex.empty()
                    ? std::string()
                    : iot::unwrap_bs_master_hex(bsKekHex, str_at(5)).value_or("");
            const std::string tenantsJson = str_at(6);   // cloud.tenants ("[]" ok)

            // Tenant-aware account planning (pure; unit-tested in
            // provisioning_policy_test.cpp). Splits ep into (tenant, serial),
            // validates a non-default tenant against cloud.tenants, scopes the
            // credential lookup to that tenant, derives identities (default ⇒
            // byte-identical to legacy), and picks the per-tenant or global
            // dm.uri. ok=false ⇒ reject the /bs.
            const iot::BsAccountPlan plan = iot::plan_bs_account(
                ep, credsJson, tenantsJson, dmUri, masterHex);
            if (!plan.ok) {
                ACE_ERROR((LM_WARNING,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l /bs for %C: no "
                                    "provisionable account (tenant=%C)\n"),
                           ep.c_str(), plan.tenant.c_str()));
                return std::nullopt;
            }
            const std::string& dmId  = plan.dm_identity;
            const std::string& dmKey = plan.dm_key;
            const std::string& bsKey = plan.bs_key;
            const bool         zeroTouch = plan.zero_touch;
            const std::string& dmUriEff = plan.dm_uri;   // per-tenant or global
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l /bs for %C: "
                                "tenant=%C serial=%C %C\n"),
                       ep.c_str(), plan.tenant.c_str(), plan.serial.c_str(),
                       zeroTouch ? "(zero-touch)" : "(commissioned)"));

            ::lwm2m::bootstrap::AccountProvisioning a;
            a.endpoint = ep;

            // Security /0/0 — the Bootstrap-Server's own account (Is
            // Bootstrap=true, SSID 0 is ignored for a BS account). PSK over
            // coaps, matching how the device actually reaches lwm2m-bs. The
            // BS DTLS identity is derived: sha256(endpoint)[:32], same as the
            // device + register_endpoint_creds compute. Written only when the
            // BS URI is known.
            if (!bsUri.empty()) {
                ::lwm2m::bootstrap::SecurityInstance bs;
                bs.iid               = 0;
                bs.serverUri         = bsUri;
                bs.isBootstrapServer = true;
                bs.securityMode      = 0;       // PSK
                // Bare canonical BS identity sha256(serial)[:32] (Option B —
                // device-agnostic, byte-identical to single-tenant); zero-touch
                // ⇒ the raw serial the device presents. From plan_bs_account.
                bs.identity          = plan.bs_identity;
                bs.secretKey         = bsKey;   // hex (omitted by encoder if empty)
                bs.shortServerId     = 0;       // ignored for a BS account
                a.security.push_back(std::move(bs));
            }

            // DM Short Server ID (1..65534). The Security RID 10 and the
            // Server Object instance are both keyed by it, so /0/1/10 links
            // directly to /1/<ssid>. v1 is single-DM-server, so one SSID.
            const std::uint16_t dmSsid = 101;

            // Security /0/1 — the DM-Server account (Is Bootstrap=false).
            ::lwm2m::bootstrap::SecurityInstance dm;
            dm.iid               = 1;
            dm.serverUri         = dmUriEff;
            dm.isBootstrapServer = false;
            dm.securityMode      = 0;           // PSK
            dm.identity          = dmId;        // distinct DM identity
            dm.secretKey         = dmKey;       // hex; client + DM both hex-decode
            dm.shortServerId     = dmSsid;
            a.security.push_back(std::move(dm));

            // Server /1/<ssid> — the DM server object, instance id == SSID so
            // it links directly to the Security RID 10 above (/0/1 → /1/101).
            ::lwm2m::bootstrap::ServerInstance srv;
            srv.iid           = dmSsid;
            srv.shortServerId = dmSsid;
            srv.lifetime      = lifetime;
            srv.binding       = binding;
            a.server.push_back(std::move(srv));

            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l /bs for %C: "
                                "provisioning BS(/0/0)+DM(/0/1) accounts "
                                "(bs=%C, dm=%C, ssid=%u, lifetime=%u)\n"),
                       ep.c_str(), bsUri.c_str(), dmUri.c_str(),
                       static_cast<unsigned>(dmSsid),
                       static_cast<unsigned>(lifetime)));
            return a;
        });
    }

    // L9 / FUP-3: attach BOTH handlers to EVERY server-side service
    // context. processRequest's URI dispatch routes /bs → bsServer,
    // /rd → regServer regardless of which socket the datagram arrived
    // on. This lets a single-socket deployment (where BS and DM share
    // a port, or one of the binds failed with EADDRINUSE) still serve
    // both Bootstrap and Registration. A future multi-socket deploy
    // gets both handlers per socket for free.
    auto regServer = std::make_shared<::lwm2m::RegistrationServer>(registry);
    // v2 Send (POST /dp) receiver — additive; /dp matches no existing route.
    auto sendServer = std::make_shared<::lwm2m::SendServer>();
    auto& services = app->udpAdapter()->services();
    for (auto& [type, ctx] : services) {
        ctx->coapAdapter()->bootstrapServer(bsServer);
        ctx->coapAdapter()->registrationServer(regServer);
        // Accept + ACK + log inbound Sends. Persistence to a telemetry store is
        // deferred (design TBD — apps/docs/tdd-vehicle-telemetry.md §3b: feed
        // the existing cloud.vehicle.telemetry spool vs a parallel inbox); this
        // wiring proves the server receive path without prejudging that choice.
        ctx->coapAdapter()->sendServer(sendServer);
        ctx->coapAdapter()->onSendReport(
            [](const std::string& base,
               const std::vector<::lwm2m::telemetry::Sample>& samples,
               const std::string& peer, std::uint16_t port) {
                ACE_DEBUG((LM_INFO,
                    ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Send /dp report base=%C "
                             "samples=%u peer=%C:%d (persist deferred)\n"),
                    base.c_str(), static_cast<unsigned>(samples.size()),
                    peer.c_str(), static_cast<int>(port)));
            });
    }

    // Online/offline endpoint state. Only the DM instance sees /rd traffic,
    // so only it publishes. lwm2m-dm is the SOLE writer of
    // cloud.lwm2m.registrations (the set of currently-registered endpoints);
    // iot-cloudd reads it and merges online/offline + last_seen into
    // cloud.endpoints (which it owns alongside tun_ip/proxy_port). A separate
    // key avoids a two-writer clobber. The set is republished on every
    // Register / Update / Deregister (via on_event) and on lifetime lapse
    // (via registry->expire in the tick below). `publish_regs` stays null for
    // non-DM instances, which skips publishing entirely.
    std::shared_ptr<std::function<void()>> publish_regs;

    // Installed firmware version per endpoint, captured from server-initiated
    // Read /3/0/3 (Firmware Version = iot.version) replies and merged into
    // cloud.lwm2m.registrations by publish_regs. `seqToEp` correlates an async
    // reply back to the endpoint it was read from — the reply only echoes our
    // token, so the tick stamps a 0x06-tagged 24-bit sequence into the token
    // and the response handler maps it back. Mutex-guarded because the tick
    // (writer) and the response handler (reader) may run on different reactor
    // threads. Allocated for every instance but only wired for dm (publish_regs
    // null elsewhere), so non-DM instances issue no version reads.
    auto epVersions = std::make_shared<std::unordered_map<std::string, std::string>>();
    auto epLanIps   = std::make_shared<std::unordered_map<std::string, std::string>>();
    // Vehicle telemetry: latest GPS lat/lon per endpoint, read from Object 6
    // (/6/0/0, /6/0/1) on the same token-tagged server-Read scheme as lan_ip,
    // and published to cloud.vehicle.telemetry for the cloud-ui Fleet Map.
    auto epGpsLat   = std::make_shared<std::unordered_map<std::string, std::string>>();
    auto epGpsLon   = std::make_shared<std::unordered_map<std::string, std::string>>();
    // Vehicle signals (Object 33000) enriching the map rows. The (tag, rid,
    // field) table is iterated by BOTH the tick (issues the Read) and the
    // response handler (routes the reply) — static, so the lambdas reference it
    // without a capture. Values land in a nested map[endpoint][field].
    auto epVeh = std::make_shared<std::unordered_map<std::string,
                          std::unordered_map<std::string, std::string>>>();
    static const struct { std::uint8_t tag; int rid; const char* field; } kVehReads[] = {
        {0x0A, 0,  "speed"},    {0x0B, 1, "rpm"},      {0x0C, 2, "coolant"},
        {0x0D, 10, "link"},     {0x0E, 3, "throttle"}, {0x0F, 4, "load"},
        {0x10, 5,  "fuel"},     {0x11, 6, "iat"},      {0x12, 7, "maf"},
        {0x13, 8,  "dtc"},
    };
    auto seqToEp    = std::make_shared<std::unordered_map<std::uint32_t, std::string>>();
    auto verMtx     = std::make_shared<std::mutex>();
    auto verSeq     = std::make_shared<std::uint32_t>(0);

    if (lwm2mInstance == "dm") {
        auto* dsClient = ds.client();
        auto lastSeen =
            std::make_shared<std::unordered_map<std::string, std::int64_t>>();
        auto now_unix = []() {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        };
        publish_regs = std::make_shared<std::function<void()>>(
            [registry, dsClient, lastSeen, now_unix, epVersions, epLanIps,
             epGpsLat, epGpsLon, epVeh, verMtx]() {
                if (!dsClient) return;
                const std::int64_t nowUnix = now_unix();
                nlohmann::json arr = nlohmann::json::array();
                std::unordered_map<std::string, std::int64_t> kept;
                for (const auto& kv : registry->all()) {
                    const auto& ep = kv.second.endpoint;
                    if (ep.empty()) continue;
                    auto it = lastSeen->find(ep);
                    const std::int64_t ls =
                        (it != lastSeen->end()) ? it->second : nowUnix;
                    kept[ep] = ls;
                    nlohmann::json row = {{"endpoint", ep},
                                          {"registered", true},
                                          {"last_seen_unix", ls},
                                          // Heartbeat interval (registration
                                          // lifetime, seconds) + the assigned
                                          // /rd/<id> location — surfaced in the
                                          // cloud-UI Endpoints table.
                                          {"lifetime", kv.second.lifetime},
                                          {"location", kv.second.location}};
                    // Carry the last-read installed version + LAN IP, if any.
                    {
                        std::lock_guard<std::mutex> lk(*verMtx);
                        auto vit = epVersions->find(ep);
                        if (vit != epVersions->end() && !vit->second.empty())
                            row["version"] = vit->second;
                        auto lit = epLanIps->find(ep);
                        if (lit != epLanIps->end() && !lit->second.empty())
                            row["lan_ip"] = lit->second;
                    }
                    // Device public/ISP IP = the DTLS registration peer address
                    // (the NAT public IP the device's DIRECT Register/Update
                    // arrives from, :5683). Captured here on the LwM2M plane so
                    // it is VPN-INDEPENDENT and, carried in the reg record, is
                    // retained across offline like lan_ip/version — vs the old
                    // OpenVPN-status source that cleared when the tunnel dropped.
                    if (!kv.second.peerHost.empty())
                        row["isp_ip"] = kv.second.peerHost;
                    arr.push_back(std::move(row));
                }
                *lastSeen = std::move(kept);
                dsClient->set(std::string("cloud.lwm2m.registrations"),
                              data_store::Value{arr.dump()});

                // Vehicle telemetry plane: a row per endpoint that has a GPS fix
                // (Object 6), read on the tick below. The cloud-ui Fleet Map
                // reads cloud.vehicle.telemetry live. Volatile (latest-wins).
                nlohmann::json varr = nlohmann::json::array();
                {
                    std::lock_guard<std::mutex> lk(*verMtx);
                    for (const auto& kv : registry->all()) {
                        const auto& ep = kv.second.endpoint;
                        if (ep.empty()) continue;
                        auto la = epGpsLat->find(ep);
                        auto lo = epGpsLon->find(ep);
                        if (la == epGpsLat->end() || lo == epGpsLon->end()) continue;
                        if (la->second.empty() || lo->second.empty()) continue;
                        nlohmann::json row = {{"endpoint", ep},
                                              {"lat", la->second},
                                              {"lon", lo->second}};
                        auto vit = epVeh->find(ep);   // enrich with Object-33000 signals
                        if (vit != epVeh->end())
                            for (const auto& f : vit->second) row[f.first] = f.second;
                        varr.push_back(std::move(row));
                    }
                }
                dsClient->set_volatile(std::string("cloud.vehicle.telemetry"),
                                       data_store::Value{varr.dump()});
            });

        regServer->on_event(
            [lastSeen, publish_regs, now_unix]
            (const ::lwm2m::RegistrationOutcome& o,
             const ::lwm2m::ServerRegistration* snap) {
                if (snap &&
                    (o.kind == ::lwm2m::RegistrationOutcome::Created ||
                     o.kind == ::lwm2m::RegistrationOutcome::Updated)) {
                    (*lastSeen)[snap->endpoint] = now_unix();
                }
                (*publish_regs)();   // covers Created / Updated / Removed
            });

        // Seed an initial (empty) snapshot so iot-cloudd starts clean.
        (*publish_regs)();

        // Capture server-initiated Read /3/0/3 replies → installed version.
        // A reply is a response-class CoAP code; processRequest routes it here
        // (m_dmRspHandler) instead of dispatching it as a request. The reply
        // echoes our token (0x06 tag + 24-bit seq); map the seq back to its
        // endpoint, record the plain-text payload (the Firmware Version
        // string), and republish so iot-cloudd + the UI pick it up.
        for (auto& [type, ctx] : services) {
            (void)type;
            ctx->coapAdapter()->dmResponseHandler(
                [epVersions, epLanIps, epGpsLat, epGpsLon, epVeh, seqToEp, verMtx, publish_regs]
                (const CoAPAdapter::CoAPMessage& m) {
                    const auto& tok = m.tokens;
                    // Token tag selects which read reply this is: 0x06 /3/0/3 (fw
                    // version), 0x07 /4/0/4 (LAN IP), 0x08 /6/0/0 (GPS lat),
                    // 0x09 /6/0/1 (GPS lon), 0x0A..0x13 Object-33000 vehicle
                    // signals (kVehReads). All stamp a 24-bit seq → endpoint.
                    if (tok.size() < 4 || tok[0] < 0x06 || tok[0] > 0x13) return;
                    const std::uint8_t  kind = tok[0];
                    const std::uint32_t seq =
                        (static_cast<std::uint32_t>(tok[1]) << 16) |
                        (static_cast<std::uint32_t>(tok[2]) << 8)  |
                         static_cast<std::uint32_t>(tok[3]);
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lk(*verMtx);
                        auto it = seqToEp->find(seq);
                        if (it == seqToEp->end()) return;
                        const std::string ep = it->second;
                        seqToEp->erase(it);
                        if (kind >= 0x0A) {
                            // Object-33000 vehicle signal → nested map[ep][field].
                            const char* field = nullptr;
                            for (const auto& vr : kVehReads)
                                if (vr.tag == kind) { field = vr.field; break; }
                            if (field) {
                                auto& slot = (*epVeh)[ep][field];
                                if (!m.payload.empty() && slot != m.payload) {
                                    slot = m.payload;
                                    changed = true;
                                }
                            }
                        } else {
                            std::unordered_map<std::string, std::string>* target = nullptr;
                            switch (kind) {
                                case 0x06: target = epVersions.get(); break;
                                case 0x07: target = epLanIps.get();   break;
                                case 0x08: target = epGpsLat.get();   break;
                                case 0x09: target = epGpsLon.get();   break;
                                default:   return;
                            }
                            if (!m.payload.empty() && (*target)[ep] != m.payload) {
                                (*target)[ep] = m.payload;
                                changed = true;
                            }
                        }
                    }
                    if (changed && publish_regs) (*publish_regs)();
                });
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

    // VPN cert delivery over Object 2048, with a VPN-connected stop signal.
    // Each tick the DM server pushes the cert family to a registered device
    // until its VPN tunnel comes up — observed cloud-side via
    // cloud.vpn.connected (iot-cloudd reads the openvpn management interface).
    // The tunnel-up state is the real "done": it needs no server→device read
    // and proves the cert is not just applied but working. Re-mint drops the
    // tunnel, which resumes the push.
    auto* dsCertPush = ds.client();

    // OTA: the campaign id (cloud.update.pending "cid") we last pushed to each
    // endpoint, so the Object-5 write+execute fires at-most-once PER CAMPAIGN —
    // re-running opkg every tick would be wrong, but a NEW push (operator
    // re-pushes the same or a new version → fresh cid from cloud.update.seq)
    // MUST re-send. Bounded to one entry per endpoint (overwritten), unlike the
    // old grow-only endpoint@version set that made a same-version re-push a
    // no-op until this daemon restarted.
    auto otaSent = std::make_shared<std::map<std::string, std::string>>();

    app->udpAdapter()->on_tick_server([registry, wapp_srv, last_poll,
                                       publish_regs, dsCertPush, otaSent,
                                       seqToEp, verMtx, verSeq, epVersions]() {
        // Local constexpr inside the lambda body — gcc 11 wouldn't let
        // us reference an outer-scope constexpr without an explicit
        // capture even though it's a constant expression.
        constexpr auto kPollInterval = std::chrono::seconds(30);

        auto a = wapp_srv.lock();
        if (!a) return;
        const auto now = Clock::now();

        // Pump tinydtls' handshake retransmission on the SERVER side too. The
        // client tick does this (on_tick_client), but the server never did — so a
        // lost cloud→device handshake flight (e.g. ServerHelloDone) was NEVER
        // retransmitted. The device would retransmit its earlier flight, the
        // server would reject it as a duplicate ("message sequence number too
        // small, expected 2, got 1"), and the DM handshake deadlocked until the
        // device restarted onto a fresh source port (a new peer). Pumping it here
        // makes the handshake self-heal on packet loss — symmetric with the
        // client, and covers EVERY peer in the server's DTLS context at once.
        for (auto& kv : a->udpAdapter()->services()) {
            if (kv.second && kv.second->dtlsAdapter())
                kv.second->dtlsAdapter()->check_retransmit();
        }

        auto expired = registry->expire(now);
        if (!expired.empty()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l expired %u registration(s)\n"),
                       static_cast<unsigned>(expired.size())));
            // Mark the now-lapsed endpoints offline by republishing the
            // (shrunk) registered set. iot-cloudd reconciles cloud.endpoints.
            if (publish_regs) (*publish_regs)();
        }

        if (now - *last_poll < kPollInterval) return;
        *last_poll = now;

        // Read the credential array once per tick. lwm2m-dm runs as cloud-svc
        // so the read_acl on cloud.endpoint.credentials lets it in.
        std::string credsJson;
        if (dsCertPush) {
            std::vector<data_store::Client::GetResult> got;
            if (dsCertPush->get({std::string("cloud.endpoint.credentials")}, got).ok
                && !got.empty() && got[0].has_value) {
                if (auto s = data_store::to_string(got[0].value)) credsJson = *s;
            }
        }

        // VPN server endpoint pushed to the device alongside the cert family,
        // so the tunnel target is cloud-provisioned (no docker-compose seed).
        // Host = the cloud's public host (parsed from cloud.dm.uri the device
        // already registers against); port/proto = the openvpn server's
        // listen config (published to ds by iot-cloudd). proto is mapped to
        // the client form (tcp-server → tcp-client). Empty host → cert-only
        // push (back-compat).
        std::string vpnHost, vpnPort, vpnProto;
        if (dsCertPush) {
            std::vector<data_store::Client::GetResult> g;
            if (dsCertPush->get({std::string("cloud.dm.uri"),
                                 std::string("cloud.vpn.listen.port"),
                                 std::string("cloud.vpn.proto")}, g).ok
                && g.size() >= 3) {
                if (g[0].has_value)
                    if (auto s = data_store::to_string(g[0].value)) {
                        // Strip scheme + port/path → bare host.
                        std::string u = *s;
                        auto p = u.find("://");
                        if (p != std::string::npos) u = u.substr(p + 3);
                        u = u.substr(0, u.find_first_of(":/"));
                        vpnHost = u;
                    }
                if (g[1].has_value)
                    if (auto n = data_store::to_int32(g[1].value))
                        vpnPort = std::to_string(*n);
                if (g[2].has_value)
                    if (auto s = data_store::to_string(g[2].value))
                        vpnProto = (*s == "tcp-server") ? "tcp-client" : *s;
            }
        }
        // Pull one endpoint's complete VPN cert family out of the array.
        auto vpn_family = [&credsJson](const std::string& ep, std::string& ca,
                                       std::string& cert, std::string& key) -> bool {
            if (credsJson.empty()) return false;
            try {
                auto arr = nlohmann::json::parse(credsJson);
                if (!arr.is_array()) return false;
                for (const auto& e : arr) {
                    if (!e.is_object() || e.value("serial", "") != ep) continue;
                    ca   = e.value("vpn.ca.cert",     "");
                    cert = e.value("vpn.client.cert", "");
                    key  = e.value("vpn.client.key",  "");
                    return !ca.empty() && !cert.empty() && !key.empty();
                }
            } catch (const std::exception&) {}
            return false;
        };

        // Devices whose VPN tunnel is already up (iot-cloudd publishes this
        // from the openvpn management interface) — the stop signal for the push.
        std::set<std::string> connected;
        if (dsCertPush) {
            std::vector<data_store::Client::GetResult> got;
            if (dsCertPush->get({std::string("cloud.vpn.connected")}, got).ok
                && !got.empty() && got[0].has_value) {
                if (auto s = data_store::to_string(got[0].value)) {
                    try {
                        auto arr = nlohmann::json::parse(*s);
                        if (arr.is_array())
                            for (const auto& e : arr)
                                if (e.is_string()) connected.insert(e.get<std::string>());
                    } catch (const std::exception&) {}
                }
            }
        }

        // Drop last tick's unanswered version-read correlations before issuing
        // fresh ones — replies arrive within seconds, so a 30s-old seq is stale
        // (this also bounds seqToEp for never-replying offline endpoints).
        if (publish_regs && seqToEp && verMtx) {
            std::lock_guard<std::mutex> lk(*verMtx);
            seqToEp->clear();
        }

        // Walk services to find the DeviceMgmtServer ServiceContext;
        // its send_async takes an explicit peer so one outbound socket
        // serves every registered client.
        for (auto& kv : a->udpAdapter()->services()) {
            if (kv.first == ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
            auto& ctx = kv.second;
            for (const auto& reg_kv : registry->all()) {
                const auto& reg = reg_kv.second;
                if (reg.peerHost.empty()) continue;

                // L9 liveness poll: Read /3/0/0.
                std::string token{static_cast<char>(0x03)};
                auto req = ::lwm2m::dmsrv::build_read(
                    next_msgid(), token,
                    /*oid*/ 3, /*iid*/ 0, /*rid*/ 0,
                    /*accept*/ -1);
                ctx->send_async(req, reg.peerHost, reg.peerPort);

                // Read /3/0/3 (Firmware Version) → installed version for the
                // cloud Software-Update "Installed" column. dm only. The reply
                // only echoes our token, so stamp a 0x06 tag + 24-bit seq and
                // map the seq back to this endpoint in the dmResponseHandler.
                if (publish_regs && verSeq && seqToEp && verMtx) {
                    std::uint32_t seq;
                    {
                        std::lock_guard<std::mutex> lk(*verMtx);
                        seq = (++(*verSeq)) & 0x00FFFFFFu;
                        (*seqToEp)[seq] = reg.endpoint;
                    }
                    std::string vtok;
                    vtok.push_back(static_cast<char>(0x06));
                    vtok.push_back(static_cast<char>((seq >> 16) & 0xFF));
                    vtok.push_back(static_cast<char>((seq >> 8)  & 0xFF));
                    vtok.push_back(static_cast<char>( seq        & 0xFF));
                    auto vreq = ::lwm2m::dmsrv::build_read(
                        next_msgid(), vtok,
                        /*oid*/ 3, /*iid*/ 0, /*rid*/ 3,
                        /*accept*/ -1);
                    ctx->send_async(vreq, reg.peerHost, reg.peerPort);

                    // Read /4/0/4 (Connectivity Monitoring IP Addresses) → the
                    // device's LAN IP for the Endpoints table. Same correlation
                    // scheme, distinguished by a 0x07 token tag.
                    std::uint32_t lseq;
                    {
                        std::lock_guard<std::mutex> lk(*verMtx);
                        lseq = (++(*verSeq)) & 0x00FFFFFFu;
                        (*seqToEp)[lseq] = reg.endpoint;
                    }
                    std::string ltok;
                    ltok.push_back(static_cast<char>(0x07));
                    ltok.push_back(static_cast<char>((lseq >> 16) & 0xFF));
                    ltok.push_back(static_cast<char>((lseq >> 8)  & 0xFF));
                    ltok.push_back(static_cast<char>( lseq        & 0xFF));
                    auto lreq = ::lwm2m::dmsrv::build_read(
                        next_msgid(), ltok,
                        /*oid*/ 4, /*iid*/ 0, /*rid*/ 4,
                        /*accept*/ -1);
                    ctx->send_async(lreq, reg.peerHost, reg.peerPort);

                    // Read /6/0/0 (GPS lat, tag 0x08) + /6/0/1 (GPS lon, tag 0x09)
                    // → cloud.vehicle.telemetry for the Fleet Map. Same token-tag
                    // correlation as lan_ip. A device with no GPS replies empty/
                    // 4.04 → no row, which the map tolerates.
                    for (const auto& gp : {std::make_pair(0x08, 0), std::make_pair(0x09, 1)}) {
                        std::uint32_t gseq;
                        {
                            std::lock_guard<std::mutex> lk(*verMtx);
                            gseq = (++(*verSeq)) & 0x00FFFFFFu;
                            (*seqToEp)[gseq] = reg.endpoint;
                        }
                        std::string gtok;
                        gtok.push_back(static_cast<char>(gp.first));
                        gtok.push_back(static_cast<char>((gseq >> 16) & 0xFF));
                        gtok.push_back(static_cast<char>((gseq >> 8)  & 0xFF));
                        gtok.push_back(static_cast<char>( gseq        & 0xFF));
                        auto greq = ::lwm2m::dmsrv::build_read(
                            next_msgid(), gtok,
                            /*oid*/ 6, /*iid*/ 0, /*rid*/ gp.second,
                            /*accept*/ -1);
                        ctx->send_async(greq, reg.peerHost, reg.peerPort);
                    }

                    // Read the Object-33000 vehicle signals (speed/rpm/coolant/
                    // link) so the map popup shows live telemetry, not just
                    // position. Same token-tag scheme (kVehReads). A device with
                    // no Vehicle object replies empty/4.04 → field simply absent.
                    for (const auto& vr : kVehReads) {
                        std::uint32_t vseq;
                        {
                            std::lock_guard<std::mutex> lk(*verMtx);
                            vseq = (++(*verSeq)) & 0x00FFFFFFu;
                            (*seqToEp)[vseq] = reg.endpoint;
                        }
                        std::string vtok2;
                        vtok2.push_back(static_cast<char>(vr.tag));
                        vtok2.push_back(static_cast<char>((vseq >> 16) & 0xFF));
                        vtok2.push_back(static_cast<char>((vseq >> 8)  & 0xFF));
                        vtok2.push_back(static_cast<char>( vseq        & 0xFF));
                        auto vreq2 = ::lwm2m::dmsrv::build_read(
                            next_msgid(), vtok2,
                            /*oid*/ 33000, /*iid*/ 0, /*rid*/ vr.rid,
                            /*accept*/ -1);
                        ctx->send_async(vreq2, reg.peerHost, reg.peerPort);
                    }
                }

                // OTA: push a pending firmware update over Object 5 (Package
                // URI WRITE /5/0/1 then Update EXECUTE /5/0/2), at-most-once
                // per endpoint+version. The device pulls the .ipk from the
                // cloud feed and applies it via opkg. Runs before the cert
                // continue so it reaches already-connected devices.
                if (dsCertPush) {
                    std::vector<data_store::Client::GetResult> pg;
                    if (dsCertPush->get({std::string("cloud.update.pending")}, pg).ok
                        && !pg.empty() && pg[0].has_value) {
                        if (auto s = data_store::to_string(pg[0].value)) {
                            try {
                                auto arr = nlohmann::json::parse(*s);
                                if (arr.is_array()) for (const auto& job : arr) {
                                    if (job.value("endpoint", "") != reg.endpoint) continue;
                                    std::string url = job.value("url", "");
                                    std::string ver = job.value("version", "");
                                    // Campaign id: a fresh push (operator re-pushes
                                    // → new cloud.update.seq) yields a new cid, so
                                    // even a same-version re-push re-sends. Fallback
                                    // to version for pre-cid pending rows.
                                    std::string cid = job.contains("cid")
                                        ? std::to_string(job.value("cid", 0))
                                        : ver;
                                    if (url.empty()) break;
                                    auto prev = otaSent->find(reg.endpoint);
                                    if (prev != otaSent->end() && prev->second == cid) break;

                                    // Downgrade guard: never push a job that is not
                                    // strictly NEWER (semver major.minor.patch) than
                                    // the device's last-read /3/0/3 version. Stops the
                                    // cloud re-installing the SAME build or clobbering
                                    // a freshly-flashed unit with a stale-but-same/older
                                    // bundle (the downgrade loop). +build metadata is
                                    // ignored. Unknown installed version → DEFER (fail
                                    // closed): the cloud Reads /3/0/3 right after
                                    // registration, so a later pass pushes once the
                                    // version is known. A blind push ~2 s after register
                                    // is exactly what shipped a 1.2.0 bundle to a 1.3.0
                                    // unit and wedged opkg on the cross-version deps.
                                    {
                                        std::string installed;
                                        {
                                            std::lock_guard<std::mutex> lk(*verMtx);
                                            auto vit = epVersions->find(reg.endpoint);
                                            if (vit != epVersions->end()) installed = vit->second;
                                        }
                                        auto semv = [](const std::string& v) -> long {
                                            long a = 0, b = 0, c = 0;
                                            std::sscanf(v.c_str(), "%ld.%ld.%ld", &a, &b, &c);
                                            return a * 1000000L + b * 1000L + c;
                                        };
                                        // Defer only a strict semver DOWNGRADE (the
                                        // 1.2.0→1.3.0 opkg wedge) or the EXACT same build
                                        // already installed (full version incl. +gitsha
                                        // matches — avoids a re-push loop). A same-semver
                                        // REBUILD (1.3.3 vs 1.3.3+abc123 → different build)
                                        // must still push: only the +gitsha changes per
                                        // build, and the device-side guard already permits
                                        // the re-install. installed unknown → defer (fail
                                        // closed; the cloud Reads /3/0/3 then retries).
                                        if (!ver.empty() &&
                                            (installed.empty() ||
                                             ver == installed ||
                                             semv(ver) < semv(installed))) {
                                            ACE_DEBUG((LM_INFO,
                                                ACE_TEXT("%D lwm2m:thread:%t %M %N:%l deferring OTA to "
                                                         "%C: ver=%C is a downgrade of / identical "
                                                         "build to installed=%C\n"),
                                                reg.endpoint.c_str(), ver.c_str(),
                                                installed.empty() ? "?(unread)" : installed.c_str()));
                                            break;
                                        }
                                    }
                                    std::string utok{static_cast<char>(0x05)};
                                    auto w = ::lwm2m::dmsrv::build_write(
                                        next_msgid(), utok, /*oid*/5, /*iid*/0,
                                        /*rid*/1, /*cf text/plain*/0, url, false);
                                    ctx->send_async(w, reg.peerHost, reg.peerPort);
                                    auto ex = ::lwm2m::dmsrv::build_execute(
                                        next_msgid(), utok, 5, 0, 2, "");
                                    ctx->send_async(ex, reg.peerHost, reg.peerPort);
                                    (*otaSent)[reg.endpoint] = cid;
                                    ACE_DEBUG((LM_INFO,
                                        ACE_TEXT("%D lwm2m:thread:%t %M %N:%l pushed OTA "
                                                 "update to %C ver=%C peer=%C:%u\n"),
                                        reg.endpoint.c_str(), ver.c_str(),
                                        reg.peerHost.c_str(),
                                        static_cast<unsigned>(reg.peerPort)));
                                    // One-shot: consume the job. Remove it from
                                    // cloud.update.pending so the cloud NEVER
                                    // re-pushes on a later registration or after a
                                    // cloud restart (otaSent is in-memory and was
                                    // lost on restart → the old re-push "on its
                                    // own"). The cloud only pushes as a direct
                                    // result of an operator queueing a job;
                                    // delivery consumes it. The operator re-pushes
                                    // to retry a missed device.
                                    {
                                        nlohmann::json remaining =
                                            nlohmann::json::array();
                                        for (const auto& j : arr) {
                                            if (j.value("endpoint", "") ==
                                                    reg.endpoint &&
                                                j.value("url", "") == url)
                                                continue;   // drop the delivered one
                                            remaining.push_back(j);
                                        }
                                        dsCertPush->set(
                                            "cloud.update.pending",
                                            data_store::Value{remaining.dump()});
                                    }
                                    // Reflect "updating" in cloud.update.status.
                                    std::vector<data_store::Client::GetResult> sg;
                                    nlohmann::json st = nlohmann::json::array();
                                    if (dsCertPush->get({std::string("cloud.update.status")}, sg).ok
                                        && !sg.empty() && sg[0].has_value)
                                        if (auto ss = data_store::to_string(sg[0].value))
                                            try { auto p = nlohmann::json::parse(*ss);
                                                  if (p.is_array()) st = p; }
                                            catch (const std::exception&) {}
                                    bool found = false;
                                    for (auto& row : st)
                                        if (row.value("serial", "") == reg.endpoint) {
                                            row["state"] = 3; found = true; break;
                                        }
                                    if (!found) st.push_back({{"serial", reg.endpoint},
                                        {"state", 3}, {"result", 0}, {"version", ver}, {"ts", 0}});
                                    dsCertPush->set("cloud.update.status",
                                                    data_store::Value{st.dump()});
                                    break;
                                }
                            } catch (const std::exception&) {}
                        }
                    }
                }

                // Push the VPN cert family over Object 2048 until the device's
                // tunnel is up. The device stages each WRITE and the Apply
                // EXECUTE materialises the family; its cert sidecar reloads
                // openvpn. Once cloud.vpn.connected reports this endpoint, the
                // cert is applied AND working, so we stop pushing.
                if (connected.count(reg.endpoint)) continue;   // tunnel up → done
                std::string ca, cert, key;
                if (vpn_family(reg.endpoint, ca, cert, key)) {
                    std::string ctok{static_cast<char>(0x04)};
                    for (auto& fr : ::lwm2m::dmsrv::build_cert_push(
                             next_msgid, ctok, ca, cert, key,
                             vpnHost, vpnPort, vpnProto))
                        ctx->send_async(fr, reg.peerHost, reg.peerPort);
                    ACE_DEBUG((LM_INFO,
                        ACE_TEXT("%D lwm2m:thread:%t %M %N:%l pushed VPN cert to "
                                 "%C (tunnel not up yet) peer=%C:%u\n"),
                        reg.endpoint.c_str(), reg.peerHost.c_str(),
                        static_cast<unsigned>(reg.peerPort)));
                }
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

/// Client bootstrap progress. The client sends POST /bs first; once the
/// Bootstrap-Server delivers + the client commits the DM account, on_done
/// switches the DTLS peer/identity to the DM and the tick registers there.
/// Skipped is the plain-DM / no-BS fallback (or a bootstrap timeout).
enum class BootPhase { NotStarted, InProgress, Done, Skipped };

/// Pet systemd's software watchdog with one `WATCHDOG=1` datagram.
///
/// Background: the LwM2M client once wedged *alive-but-inert* after a
/// VPN-reconnect re-Register — the 1 Hz reactor tick stopped, the DM
/// socket was gone, yet the process never crashed, so systemd (Type=simple)
/// still saw it "running" and never restarted it. The device sat offline
/// for ~13 h. A systemd watchdog catches that whole class of failure: the
/// unit sets WatchdogSec= and the client tick calls this every second; if
/// the reactor ever stalls the pings stop and systemd kills + restarts it.
///
/// We write the sd_notify wire protocol by hand (a datagram to the AF_UNIX
/// socket named by $NOTIFY_SOCKET) rather than linking libsystemd, so the
/// dependency surface is unchanged. systemd exports NOTIFY_SOCKET whenever
/// WatchdogSec= is set; absent it (non-systemd, or the server role whose
/// unit has no WatchdogSec) this is a cheap no-op. NotifyAccess defaults to
/// "main" and all our threads share the process PID, so any thread may ping.
static void systemd_watchdog_ping() {
    const char* sock = std::getenv("NOTIFY_SOCKET");
    if (sock == nullptr || (sock[0] != '/' && sock[0] != '@')) return;

    struct sockaddr_un addr;
    const std::size_t len = std::strlen(sock);
    if (len >= sizeof(addr.sun_path)) return;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, sock, len);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    static const char kMsg[] = "WATCHDOG=1";
    const socklen_t alen =
        static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + len);
    (void)::sendto(fd, kMsg, sizeof(kMsg) - 1, 0,
                   reinterpret_cast<const struct sockaddr*>(&addr), alen);
    ::close(fd);
}

/// Derive the device-ui connection-lifecycle token from the live FSM
/// signals. Published to iot.conn.state each tick (only when it changes).
/// The progression mirrors what the operator sees on the dashboard:
///   bootstrapping → bootstrapped → dm-connecting → dm-connected → registered
/// "*-connecting" means a DTLS handshake is in flight; "*-connected" means
/// the secure channel is up and the protocol exchange (BS writes / Register)
/// is underway. `dtlsReady` is true when the session to the current peer is
/// established (or there is no DTLS at all, i.e. plain CoAP).
static const char* compute_conn_state(BootPhase bp,
                                      ::lwm2m::RegistrationState rs,
                                      bool dtlsReady, bool disabled) {
    using RS = ::lwm2m::RegistrationState;
    if (rs == RS::Registered || rs == RS::AwaitingUpdateAck) return "registered";
    if (rs == RS::Failed)                                    return "failed";
    if (disabled && rs == RS::Unregistered)                  return "idle";
    switch (bp) {
        case BootPhase::NotStarted:
        case BootPhase::InProgress:
            return dtlsReady ? "bootstrapped" : "bootstrapping";
        case BootPhase::Done:
        case BootPhase::Skipped:
            return dtlsReady ? "dm-connected" : "dm-connecting";
    }
    return "idle";
}

/// Single-quote a string for safe inclusion in a /bin/sh command.
static std::string ota_shquote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}

/// Launch the detached OTA *stager* for `uri` as a systemd transient unit.
/// iot-ota-stage downloads + verifies the .ipk into /run/iot/update and touches
/// the inotify trigger; the actual opkg install runs in the separate
/// iot-swupdate.service (so it survives opkg replacing the running binaries).
/// Detached so it's never a child of this daemon. Returns 0 on launch.
/// See apps/docs/tdd-yocto-swupdate.md.
static int ota_launch_apply(const std::string& uri) {
    if (uri.empty()) return -1;
    // The lwm2m client runs UNPRIVILEGED (User=engineer) and CANNOT `systemd-run`
    // a system unit — polkit denies it ("Access denied", rc=256) and the busybox
    // image ships no polkit agent, so the stager never launches. Instead, drop
    // the package URL into the spool request file that the root iot-ota-stage.path
    // unit watches; it fires iot-ota-stage.service (root), which downloads +
    // verifies and triggers iot-swupdate. /run/iot/update is 0775 root:iot
    // (tmpfiles iot.conf) and the client has SupplementaryGroups=iot, so this
    // write needs no privilege. Written temp + rename so the .path never fires on
    // a half-written request. Same engineer->root handoff as the cert-apply ->
    // iot-vpn-cert.path path. Returns 0 once queued (the download is async).
    const std::string req = "/run/iot/update/stage.req";
    const std::string tmp = req + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        os << uri << '\n';
        if (!os) {
            ACE_ERROR((LM_ERROR,
                ACE_TEXT("%D [ota] %M %N:%l write(%C) failed errno=%d\n"),
                tmp.c_str(), errno));
            return -1;
        }
    }
    if (std::rename(tmp.c_str(), req.c_str()) != 0) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [ota] %M %N:%l rename(%C) failed errno=%d\n"),
            req.c_str(), errno));
        std::remove(tmp.c_str());
        return -1;
    }
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ota] %M %N:%l queued OTA stage request "
                 "(iot-ota-stage.path runs the stager)\n")));
    return 0;
}

/// A/B bank switch: run iot-bank-switch (root, detached) with the requested
/// target bank ("A"/"B"/"other"). It `rauc status mark-active`s that bank and
/// reboots; the bootloader rolls back if the new bank fails to come up.
/// FIXME: this still uses `systemd-run`, which FAILS for the same reason the OTA
/// path did — the client runs as engineer and polkit denies a system transient
/// unit. A/B is unvalidated Phase 2 (gated by IOT_AB), so it's left as-is; when
/// A/B is brought up, give it the same spool-trigger handoff as
/// ota_launch_apply (an iot-bank-switch.path watching /run/iot/update/bank.req).
static int bank_switch_launch(const std::string& target) {
    if (target.empty()) return -1;
    std::string cmd =
        "systemd-run --unit=iot-bank-switch --collect /usr/bin/iot-bank-switch "
        + ota_shquote(target);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [ab] %M %N:%l bank-switch launch failed rc=%d cmd=%C\n"),
            rc, cmd.c_str()));
        return -1;
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ab] %M %N:%l launched iot-bank-switch %C\n"),
        target.c_str()));
    return 0;
}

ClientPlumbing wire_client(std::shared_ptr<App>& app,
                           const std::string& endpoint,
                           const std::string& configDir,
                           const std::string& bsHost,
                           std::uint16_t bsPort,
                           iot::DsConfig& ds) {
    ClientPlumbing plumb;
    plumb.store = std::make_shared<::lwm2m::ObjectStore>();

    // ── OTA (LwM2M Object 5) apply hooks ──────────────────────────────
    // RID 3/5/7 read the live apply progress from ds (iot.update.*, written by
    // the detached iot-ota-stage + iot-swupdate); RID 2 launches the stager.
    data_store::Client* dsc = ds.client();
    ::lwm2m::objects::FwHooks fwHooks;
    fwHooks.launch = [](const std::string& uri) { return ota_launch_apply(uri); };
    fwHooks.read_state = [dsc]() -> long long {
        std::vector<data_store::Client::GetResult> got;
        if (dsc && dsc->get({"iot.update.state"}, got).ok && !got.empty() && got[0].has_value)
            if (auto n = data_store::to_int32(got[0].value)) return *n;
        return 0;
    };
    fwHooks.read_result = [dsc]() -> long long {
        std::vector<data_store::Client::GetResult> got;
        if (dsc && dsc->get({"iot.update.result"}, got).ok && !got.empty() && got[0].has_value)
            if (auto n = data_store::to_int32(got[0].value)) return *n;
        return 0;
    };
    fwHooks.read_version = [dsc]() -> std::string {
        std::vector<data_store::Client::GetResult> got;
        if (dsc && dsc->get({"iot.update.version"}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) return *s;
        return {};
    };
    // Cert apply (OID 2048): the systemd/RPi image ships no cert sidecar, so
    // when a fresh VPN cert family is materialised, gate-flip the openvpn
    // client service to make its supervisor (re)start openvpn with the new
    // cert. Watches fire on change, so toggle false→true for a real transition.
    ::lwm2m::objects::CertHooks certHooks;
    certHooks.apply = [dsc]() -> int {
        if (!dsc) return 0;
        dsc->set("services.openvpn.client.enable", data_store::Value{false});
        dsc->set("services.openvpn.client.enable", data_store::Value{true});
        return 0;
    };
    // Cloud-provisioned VPN server endpoint (Object 2048 /0/5,6,7): persist it
    // to ds so openvpn-client targets the right server with no compose seed.
    // vpn.remote.port is an integer in schema, so the decimal string is parsed.
    certHooks.set_vpn_endpoint = [dsc](const std::string& host,
                                       const std::string& port,
                                       const std::string& proto) -> int {
        if (!dsc || host.empty()) return 0;
        dsc->set("vpn.remote.host", data_store::Value{host});
        if (!port.empty()) {
            try {
                dsc->set("vpn.remote.port",
                         data_store::Value{static_cast<std::int32_t>(std::stoi(port))});
            } catch (...) {}
        }
        if (!proto.empty()) dsc->set("vpn.remote.proto", data_store::Value{proto});
        return 0;
    };
    // Device object (OID 3): bind RID 3 Firmware Version to the running
    // release (iot.version, written by httpd = IOT_VERSION) so a server Read
    // /3/0/3 surfaces what the device is actually running — the cloud uses
    // this for the Software-Update "Installed" column.
    ::lwm2m::objects::DeviceHooks deviceHooks;
    deviceHooks.firmwareVersion = [dsc]() -> std::string {
        std::vector<data_store::Client::GetResult> got;
        if (dsc && dsc->get({"iot.version"}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value))
                if (!s->empty()) return *s;
        return {};
    };
    // /4/0/4 (Connectivity Monitoring IP Addresses) → the device's IP on its
    // ACTIVE WAN interface (eth0 / wlan0 / wwan0), so the cloud Endpoints table
    // shows the address the device is actually reachable at — not hard-wired to
    // WiFi. net-router owns interface state: it picks the highest-priority
    // OPER-UP iface and publishes that iface's routable IPv4 as
    // net.iface.active.ip (bearer-agnostic, follows DHCP renews). We just read
    // that ds key — no getifaddrs / interface enumeration in the lwm2m process.
    deviceHooks.ipAddresses = [dsc]() -> std::string {
        if (!dsc) return {};
        std::vector<data_store::Client::GetResult> got;
        if (dsc->get({std::string("net.iface.active.ip")}, got).ok &&
            !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) return *s;
        return {};
    };
    // Location (OID 6): bind lat/lon/alt/speed to the gps.* ds keys published
    // by the cellular-client daemon (it owns the GNSS); a server Read/Observe
    // /6/0/* then surfaces device location to the cloud. Absent/empty → "0".
    // Timestamp (RID 5) stays static until the daemon publishes an epoch.
    ::lwm2m::objects::LocationHooks locHooks;
    {
        auto gpsVal = [dsc](const char* key) -> std::string {
            std::string v;
            if (dsc) {
                std::vector<data_store::Client::GetResult> got;
                if (dsc->get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
                    if (auto s = data_store::to_string(got[0].value)) v = *s;
            }
            return v.empty() ? std::string("0") : v;
        };
        locHooks.latitude  = [gpsVal]() { return gpsVal("gps.lat"); };
        locHooks.longitude = [gpsVal]() { return gpsVal("gps.lon"); };
        locHooks.altitude  = [gpsVal]() { return gpsVal("gps.alt"); };
        locHooks.speed     = [gpsVal]() { return gpsVal("gps.speed"); };
    }
    ::lwm2m::objects::install_canonical_objects(*plumb.store, configDir,
                                                std::move(deviceHooks),
                                                std::move(fwHooks), std::move(certHooks),
                                                std::move(locHooks));

    // Vehicle Telemetry (custom OID 33000): bind OBD-II signals to the vehicle.*
    // ds keys published by iot-vehicled (it owns the CAN bus). A server
    // Read/Observe /33000/0/* surfaces live vehicle data to the cloud map. Each
    // hook reuses the same ds reader as gps.* above; empty → static default.
    ::lwm2m::objects::VehicleHooks vehHooks;
    {
        auto vehVal = [dsc](const char* key) -> std::string {
            std::string v;
            if (dsc) {
                std::vector<data_store::Client::GetResult> got;
                if (dsc->get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
                    if (auto s = data_store::to_string(got[0].value)) v = *s;
            }
            return v;
        };
        vehHooks.speed    = [vehVal]() { return vehVal("vehicle.speed"); };
        vehHooks.rpm      = [vehVal]() { return vehVal("vehicle.rpm"); };
        vehHooks.coolant  = [vehVal]() { return vehVal("vehicle.coolant"); };
        vehHooks.throttle = [vehVal]() { return vehVal("vehicle.throttle"); };
        vehHooks.load     = [vehVal]() { return vehVal("vehicle.load"); };
        vehHooks.fuel     = [vehVal]() { return vehVal("vehicle.fuel"); };
        vehHooks.iat      = [vehVal]() { return vehVal("vehicle.iat"); };
        vehHooks.maf      = [vehVal]() { return vehVal("vehicle.maf"); };
        vehHooks.dtc      = [vehVal]() { return vehVal("vehicle.dtc"); };
        vehHooks.link     = [vehVal]() { return vehVal("vehicle.link"); };
    }
    ::lwm2m::objects::install_vehicle(*plumb.store, std::move(vehHooks));

    // ── IPSO sensor objects (mangOH Yellow) ───────────────────────────────
    // Sensor values are produced by the privileged iot-sensord daemon (it owns
    // the I2C bus / mangOH board) and published to iot.sensor.* in the
    // data-store; this (unprivileged) client mirrors them into the IPSO objects
    // 3301/3303/3304/3313/3315/3334 so a server Read/Observe surfaces them — the
    // same ds-handoff used for iot.version, net.iface.active.ip and iot.update.*.
    // An absent key reads "0", so a board without the mangOH attached still
    // advertises the objects (with zero readings) rather than erroring.
    {
        auto sensorVal = [dsc](const char* key) -> std::string {
            std::string v;
            if (dsc) {
                std::vector<data_store::Client::GetResult> got;
                if (dsc->get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
                    if (auto s = data_store::to_string(got[0].value)) v = *s;
            }
            return v.empty() ? std::string("0") : v;
        };
        // iot.sensor.accel / .gyro carry "x,y,z"; pull the idx-th CSV field.
        auto sensorCsv = [sensorVal](const char* key, int idx) -> std::string {
            std::string v = sensorVal(key);
            std::size_t start = 0;
            for (int i = 0; i < idx; ++i) {
                start = v.find(',', start);
                if (start == std::string::npos) return std::string("0");
                ++start;
            }
            std::size_t end = v.find(',', start);
            std::string f = v.substr(start, end == std::string::npos ? std::string::npos : end - start);
            return f.empty() ? std::string("0") : f;
        };
        ::lwm2m::objects::SensorHooks sh;
        sh.temperature = [sensorVal]() { return sensorVal("iot.sensor.temp"); };
        sh.humidity    = [sensorVal]() { return sensorVal("iot.sensor.humidity"); };
        sh.pressure    = [sensorVal]() { return sensorVal("iot.sensor.pressure"); };
        sh.illuminance = [sensorVal]() { return sensorVal("iot.sensor.lux"); };
        sh.accel_x = [sensorCsv]() { return sensorCsv("iot.sensor.accel", 0); };
        sh.accel_y = [sensorCsv]() { return sensorCsv("iot.sensor.accel", 1); };
        sh.accel_z = [sensorCsv]() { return sensorCsv("iot.sensor.accel", 2); };
        sh.gyro_x  = [sensorCsv]() { return sensorCsv("iot.sensor.gyro", 0); };
        sh.gyro_y  = [sensorCsv]() { return sensorCsv("iot.sensor.gyro", 1); };
        sh.gyro_z  = [sensorCsv]() { return sensorCsv("iot.sensor.gyro", 2); };
        ::lwm2m::objects::install_sensors(*plumb.store, std::move(sh));
    }

    // device-ui self-update: a write to iot.update.request launches the same
    // detached apply locally (no CoAP round-trip).
    if (dsc) {
        data_store::Client::WatchHandle wh = data_store::Client::kInvalidHandle;
        dsc->watch("iot.update.request",
            [](const data_store::Client::Event& e) {
                if (auto s = data_store::to_string(e.value)) {
                    if (!s->empty()) ota_launch_apply(*s);
                }
            }, &wh);

        // device-ui A/B bank switch: a write to iot.boot.switch.request (the
        // target bank) reboots the device into that bank. iot-bank-switch
        // clears the key, so we ignore the empty-string reset it writes back.
        data_store::Client::WatchHandle wbs = data_store::Client::kInvalidHandle;
        dsc->watch("iot.boot.switch.request",
            [](const data_store::Client::Event& e) {
                if (auto s = data_store::to_string(e.value)) {
                    if (!s->empty()) bank_switch_launch(*s);
                }
            }, &wbs);
    }

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
    // The authoritative registration lifetime is delivered by the Bootstrap
    // server in the Server Object (RID 1) — which the BS reads from
    // cloud.dm.lifetime. The client does NOT read its own data-store for it;
    // this is only the pre-bootstrap default (used if bootstrap is skipped).
    // on_done applies the bootstrap-delivered value below.
    cfg.lifetime     = 86400;
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

    // Bootstrap progress, shared between on_done (writer) and the reactor
    // tick (reader/driver). Both run on the reactor thread, so no lock.
    auto bootPhase    = std::make_shared<BootPhase>(BootPhase::NotStarted);
    auto bootDeadline =
        std::make_shared<std::chrono::steady_clock::time_point>();
    // Bootstrap retry-with-backoff so a dropped ClientHello / briefly-down BS /
    // an unanswered /bs never wedges us permanently at "bootstrapping".
    // bootRetryAfter gates the next bootstrap action; bootBackoff (seconds)
    // doubles 2→4→…→cap each failed attempt. Default-constructed time_point =
    // epoch, so the first attempt fires immediately.
    auto bootRetryAfter =
        std::make_shared<std::chrono::steady_clock::time_point>();
    auto bootBackoff  = std::make_shared<int>(2);   // seconds; doubles, cap 30

    // After the Bootstrap commit, switch the LwM2MClient peer + DTLS identity
    // to the DM and (re)connect; the tick sends Register once the DM
    // handshake completes. Runs on the reactor thread (apply_commit invoked
    // it); we only mutate state + kick the handshake, no recursion into tx.
    std::weak_ptr<App>                          wapp_bs = app;
    std::weak_ptr<::lwm2m::RegistrationClient>  wreg_bs = plumb.reg;
    plumb.bs->on_done([wapp_bs, wreg_bs, &ds, dtls, bootPhase]
                      (const ::lwm2m::bootstrap::StagingBuffer& committed) {
        // Pull the bootstrap-delivered DM account (the non-BS PSK instance):
        // server URI (RID 0) + identity (RID 3). Persist its key (RID 5) to
        // the data-store (write-only) for visibility.
        std::string dmUri, dmIdentity;
        for (const auto& s : committed.security) {
            if (s.isBootstrapServer || s.securityMode != 0 /*PSK*/) continue;
            if (s.identity.empty() || s.secretKey.empty()) continue;
            dmUri      = s.serverUri;
            dmIdentity = s.identity;
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
        if (!a) return;
        UDPAdapter::Scheme_t sc{};
        std::string   dmHost;
        std::uint16_t dmPort = 0;
        if (!dmUri.empty()) parsePeerOption(dmUri, sc, dmHost, dmPort);
        if (dmHost.empty() || dmPort == 0) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l bootstrap commit but "
                                "no usable DM URI — falling back to direct "
                                "Register\n")));
            *bootPhase = BootPhase::Skipped;
            return;
        }
        // Persist the bootstrap-delivered DM Server URI (Security Object
        // RID 0) to the data-store so the device-ui can display the URI the
        // device actually registered to (otherwise iot.dm.uri stays empty).
        if (ds.set_dm_uri(dmUri)) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l DM URI persisted to "
                                "data-store (iot.dm.uri=%C)\n"),
                       dmUri.c_str()));
        }
        // Derive the VPN server host from the DM URI: the VPN concentrator is
        // co-located with the DM on the cloud VM, so its host == dmHost. This
        // gives a co-located cloud zero VPN-host config on the device (just the
        // commissioned bootstrap URI + BS PSK). The cloud's Object-2048 endpoint
        // push (set_vpn_endpoint) still overrides this for a split topology;
        // in the common case both write the same value (idempotent).
        if (ds.set_vpn_remote_host(dmHost)) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l VPN remote host "
                                "derived from DM URI (vpn.remote.host=%C)\n"),
                       dmHost.c_str()));
        }
        // Point the LwM2MClient peer at the DM, pin the DM PSK identity, and
        // force a fresh DTLS handshake against the DM.
        for (auto& [type, ctx] : a->udpAdapter()->services()) {
            if (type != ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
            ctx->peerHost(dmHost);
            ctx->peerPort(dmPort);
        }
        if (dtls) {
            dtls->active_identity(dmIdentity);
            dtls->clientState("");            // drop the BS session state
            // Force a FRESH DM handshake. A DM peer left CONNECTED from before a
            // cloud restart (tinydtls has no liveness check, so the device never
            // noticed the server lost its peer table) makes plain connect()
            // attempt an ENCRYPTED renegotiation ClientHello. The restarted DM —
            // peerless — rejects that as "no peer available / only ClientHello
            // is allowed" and the device wedges at dm-connecting forever (only a
            // manual iot-lwm2m-client restart recovered it). reset_and_connect()
            // tears the stale peer down first so a PLAINTEXT ClientHello starts a
            // clean handshake the restarted DM accepts — auto-recovery after any
            // cloud bounce/upgrade.
            //
            // toBootstrapIdentity=FALSE: keep the DM identity pinned just above.
            // reset_and_connect() defaults to restoring the BS identity (it was
            // built for the BS re-handshake); using that default here made the DM
            // handshake present sha256(serial) — the DM server has no PSK for it,
            // so the device wedged at dm-connecting AND never registered (the OTA
            // "stuck at 90%" + offline regression from the reset_and_connect-on-
            // DM-switch fix). Keeping the DM identity is the whole point here.
            dtls->reset_and_connect(dmHost, dmPort, /*toBootstrapIdentity=*/false);
        }
        // Apply the bootstrap-delivered registration lifetime (Server Object
        // RID 1) — the client uses this, sourced by the BS from
        // cloud.dm.lifetime, rather than its own data-store.
        if (auto r = wreg_bs.lock(); r && !committed.server.empty()) {
            const std::uint32_t lt = committed.server.front().lifetime;
            r->set_lifetime(lt);
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l applied bootstrap "
                                "lifetime=%u (Server Object RID 1)\n"),
                       static_cast<unsigned>(lt)));
        }
        *bootPhase = BootPhase::Done;
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l bootstrap commit done; "
                            "switching to DM %C:%u (identity=%C) — Register on "
                            "next connected tick\n"),
                   dmHost.c_str(), static_cast<unsigned>(dmPort),
                   dmIdentity.c_str()));
    });

    // FUP-DS-11 — DM server URI rebind mailbox. Listener thread fills
    // it from a Key::ServerUri change; reactor tick consumes it once
    // state is cleanly Unregistered (so the Deregister to the old
    // server has been ack'd), swaps the LwM2MClient peer, then the
    // existing Unregistered branch sends Register to the new peer.
    auto rebind = std::make_shared<ClientPlumbing::Rebind>();
    plumb.rebind = rebind;

    // ── v2 Send telemetry uploader (TDD §3b #1). OFF unless
    // iot.telemetry.send.enable — otherwise `sendst->up` stays null and the tick
    // block below is a no-op (zero change to the registration path). When on, it
    // buffers numeric vehicle.* signals and pushes them as SenML packs over the
    // registered DM session (direct DTLS), with offline backfill via the
    // DurableSampleBuffer when iot.telemetry.db.path names a file.
    struct SendState {
        std::shared_ptr<::lwm2m::send::Uploader> up;
        std::chrono::steady_clock::time_point    inflight_since{};
        std::chrono::steady_clock::time_point    last_sample{};
        int sample_secs   = 5;
        int ack_timeout_s = 6;     // < the 30s update margin → no keepalive risk
    };
    auto sendst = std::make_shared<SendState>();
    {
        auto* cli = ds.client();
        std::vector<data_store::Client::GetResult> g;
        bool enable = false;
        std::string dbpath, basepath = "/33000/0/";
        int cap = 1000, maxbatch = 8, ttl = 0;
        if (cli && cli->get({std::string("iot.telemetry.send.enable"),
                             std::string("iot.telemetry.db.path"),
                             std::string("iot.telemetry.basepath"),
                             std::string("iot.telemetry.capacity"),
                             std::string("iot.telemetry.maxbatch"),
                             std::string("iot.telemetry.sample.secs"),
                             std::string("iot.telemetry.ttl.secs")}, g).ok
            && g.size() == 7) {
            if (auto b = data_store::to_bool(g[0].value))   enable   = *b;
            if (auto s = data_store::to_string(g[1].value)) dbpath   = *s;
            if (auto s = data_store::to_string(g[2].value); s && !s->empty()) basepath = *s;
            if (auto i = data_store::to_int32(g[3].value))  cap      = *i;
            if (auto i = data_store::to_int32(g[4].value))  maxbatch = *i;
            if (auto i = data_store::to_int32(g[5].value))  sendst->sample_secs = *i > 0 ? *i : 1;
            if (auto i = data_store::to_int32(g[6].value))  ttl      = *i;
        }
        if (enable) {
            sendst->up = std::make_shared<::lwm2m::send::Uploader>(
                ::lwm2m::telemetry::make_sample_buffer(
                    dbpath, static_cast<std::size_t>(cap < 1 ? 1 : cap),
                    static_cast<std::int64_t>(ttl)),
                basepath, static_cast<std::size_t>(maxbatch < 1 ? 1 : maxbatch));
            // Route the 2.04 Send ack (matched by msg-id) on every client adapter.
            std::weak_ptr<SendState> wsend = sendst;
            for (auto& [type, ctx] : app->udpAdapter()->services()) {
                (void)type;
                ctx->coapAdapter()->sendAckHandler(
                    [wsend](const CoAPAdapter::CoAPMessage& m) {
                        auto st = wsend.lock();
                        if (!st || !st->up || !st->up->in_flight()) return;
                        if (m.coapheader.msgid != st->up->in_flight_msgid()) return;
                        if ((m.coapheader.code >> 5) == 2)
                            st->up->on_ack(m.coapheader.msgid);
                        // non-2.xx → leave it; the tick's timeout requeues+retries
                    });
            }
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D lwm2m:thread:%t %M %N:%l telemetry Send enabled "
                         "(base=%C buf=%C cap=%d batch=%d every=%ds)\n"),
                basepath.c_str(), dbpath.empty() ? "ram" : dbpath.c_str(),
                cap, maxbatch, sendst->sample_secs));
        }
    }

    // 1 Hz ticker: drives Update emission + Observe pmax + (TODO) initial
    // Register once bootstrap completes.
    std::weak_ptr<::lwm2m::RegistrationClient> wreg = plumb.reg;
    // VPN-reconnect-triggered re-Register. A cloud/DM restart is otherwise only
    // noticed at the next registration Update (lifetime - margin ≈ up to 24h),
    // so the device sits offline for a long time. The openvpn client detects the
    // link drop via ping-restart (~120s) and republishes vpn.state; on a genuine
    // reconnect edge (up → dropped → up) force a re-Register so the device comes
    // back online within ~2 min. The phase guard avoids firing on the FIRST
    // connect (the normal bootstrap/Register path handles that). Runs on the ds
    // listener thread (serialised per key), same as the iot.server.uri watch;
    // request_reregister() just sets a flag the 1 Hz tick consumes, so it's safe.
    if (dsc) {
        auto vpnPhase = std::make_shared<int>(0);  // 0 pre-connect, 1 up, 2 dropped
        data_store::Client::WatchHandle wvpn = data_store::Client::kInvalidHandle;
        dsc->watch("vpn.state",
            [wreg, vpnPhase](const data_store::Client::Event& e) {
                const std::string st = data_store::to_string(e.value).value_or("");
                if (st == "connected") {
                    if (*vpnPhase == 2) {
                        if (auto reg = wreg.lock()) {
                            reg->request_reregister();
                            ACE_DEBUG((LM_INFO,
                                ACE_TEXT("%D lwm2m:thread:%t %M %N:%l VPN reconnected "
                                         "after a drop — forcing LwM2M re-Register\n")));
                        }
                    }
                    *vpnPhase = 1;
                } else if (*vpnPhase == 1) {
                    *vpnPhase = 2;   // tunnel dropped after being up
                }
            }, &wvpn);
    }
    std::weak_ptr<::lwm2m::DmClient>           wdm  = plumb.dm;
    std::weak_ptr<App>                         wapp = app;
    std::weak_ptr<::lwm2m::bootstrap::Client>  wbs  = plumb.bs;
    // Task K — cooldown so a persistently-rejecting DM doesn't spin the
    // bootstrap. Shared across ticks; default-constructed = epoch.
    auto reboot_after =
        std::make_shared<std::chrono::steady_clock::time_point>();
    // Last connection-lifecycle token published to iot.conn.state. The tick
    // runs at 1 Hz; we only re-publish on a transition so the device-ui
    // long-poll fires on real changes, not every second.
    auto lastConn = std::make_shared<std::string>();
    // NAT keepalive — last time we emitted a CoAP ping (or an Update, which
    // also holds the mapping). Epoch ⇒ first Registered tick pings immediately.
    auto lastKeepalive =
        std::make_shared<std::chrono::steady_clock::time_point>();
    app->udpAdapter()->on_tick_client([wreg, wdm, wapp, rebind, wbs,
                                       reboot_after, bootPhase, bootDeadline,
                                       bootRetryAfter, bootBackoff, bsHost, bsPort,
                                       dtls, &ds, lastConn, lastKeepalive, sendst]() {
        // Liveness first: petting the watchdog here ties it to the reactor
        // actually dispatching this 1 Hz tick. If the reactor stalls (the
        // alive-but-wedged failure this guards against), the pings stop and
        // systemd restarts the unit. No-op unless WatchdogSec= is set.
        systemd_watchdog_ping();

        auto reg = wreg.lock();
        auto dm  = wdm.lock();
        auto a   = wapp.lock();
        if (!reg || !dm || !a) return;

        const auto svc = ::UDPAdapter::ServiceType_t::LwM2MClient;
        const auto now = std::chrono::steady_clock::now();

        // Pump tinydtls' handshake retransmission so an unanswered flight
        // (e.g. the bootstrap ClientHello) is retried instead of wedging the
        // connection. Without this, a single dropped packet stalls bootstrap
        // forever.
        if (dtls) dtls->check_retransmit();

        // Recover a lost registration ack. The FSM leaves an Awaiting*Ack
        // state only on a response, so a dropped datagram (e.g. an Update lost
        // in a network blip) once stranded it forever — "registered" on-device
        // while the cloud had already expired it. Retransmit a lost Update a
        // few times; if that budget is spent the session is suspect, so replay
        // the boot path (re-handshake DTLS, re-bootstrap, re-Register).
        switch (reg->check_ack_timeout(now)) {
            case ::lwm2m::RegistrationClient::AckRecovery::RetransmitUpdate: {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l Update ack timed "
                                    "out — retransmitting\n")));
                auto payload = reg->build_update_request(
                    next_msgid(), std::string{static_cast<char>(0x02)},
                    /*withAdvertisedSet*/ false);
                tx_via(*a, payload, svc);
                break;
            }
            case ::lwm2m::RegistrationClient::AckRecovery::ReRegister:
                ACE_ERROR((LM_WARNING,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l registration ack "
                                    "timed out — restarting bootstrap to "
                                    "re-establish the session\n")));
                *bootPhase      = BootPhase::NotStarted;
                *bootBackoff    = 1;
                *bootRetryAfter = now;
                break;
            case ::lwm2m::RegistrationClient::AckRecovery::None:
                break;
        }

        // Publish the connection lifecycle for the device-ui (only on change).
        {
            const bool dtlsUp = !dtls || dtls->clientState() == "connected";
            const char* cs = compute_conn_state(*bootPhase, reg->state(),
                                                dtlsUp, reg->is_disabled());
            if (*lastConn != cs) {
                *lastConn = cs;
                ds.set_conn_state(cs);
            }
        }

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

        // Bootstrap-first startup. While Unregistered we:
        //   1. send POST /bs once (NotStarted → InProgress);
        //   2. wait for on_done to commit + switch us to the DM (→ Done),
        //      or time out and fall back to direct Register (→ Skipped);
        //   3. Register once ready — for Done that means the DM DTLS
        //      handshake (kicked in on_done) has completed.
        // The branch fires every tick while Unregistered, so step 3 retries
        // until the handshake is up; after a later Deregister it re-registers
        // without re-bootstrapping (bootPhase stays Done).
        if (reg->state() == ::lwm2m::RegistrationState::Unregistered &&
            !reg->is_disabled()) {
            auto bs = wbs.lock();
            // True once the DTLS session to the *current* peer is up — the BS
            // while NotStarted, the DM once on_done switched us over. Gating
            // both the single-shot /bs and Register on it avoids sending into
            // a half-open handshake (the frame would be dropped, not retried).
            const bool dtlsReady = !dtls || dtls->clientState() == "connected";

            if (*bootPhase == BootPhase::NotStarted) {
                if (!bs) {
                    *bootPhase = BootPhase::Skipped;        // plain-DM / no BS
                } else if (now >= *bootRetryAfter) {
                    if (dtlsReady) {
                        ACE_DEBUG((LM_INFO,
                                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l initiating "
                                            "bootstrap: POST /bs\n")));
                        auto payload = bs->build_bs_request(
                            next_msgid(), std::string{static_cast<char>(0x40)});
                        tx_via(*a, payload, svc);
                        *bootPhase    = BootPhase::InProgress;
                        *bootDeadline = now + std::chrono::seconds(15);
                    } else if (dtls) {
                        // BS DTLS isn't up yet. check_retransmit() (above) retries
                        // in-flight handshake flights; re-kick connect() too in
                        // case the peer was dropped after max retransmits, on a
                        // growing backoff. Retry forever — a gateway must
                        // eventually reach the cloud.
                        dtls->connect(bsHost, bsPort);
                        *bootRetryAfter = now + std::chrono::seconds(*bootBackoff);
                        ACE_DEBUG((LM_INFO,
                                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS DTLS not "
                                            "up — re-kicking handshake, next retry "
                                            "in %d s\n"), *bootBackoff));
                        *bootBackoff = (*bootBackoff * 2 > 30) ? 30 : (*bootBackoff * 2);
                    }
                }
                // else: within the backoff window; retry on a later tick.
            } else if (*bootPhase == BootPhase::InProgress &&
                       now >= *bootDeadline) {
                // /bs didn't complete in time. The BS DTLS session may be DEAD
                // (cloud bs restart, NAT drop, or a stale session left after a
                // watchdog restart) while clientState still reads "connected" —
                // re-POSTing /bs over it transmits NOTHING and loops forever, and
                // because the reactor keeps ticking the watchdog never catches it
                // (observed: a device stuck "bootstrapping" 10h that a manual
                // restart fixed in ~1s). Force a fresh BS handshake so the retry
                // actually reaches the cloud instead of POSTing into the void.
                if (dtls) dtls->reset_and_connect(bsHost, bsPort);
                *bootRetryAfter = now + std::chrono::seconds(*bootBackoff);
                ACE_ERROR((LM_WARNING,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l bootstrap did not "
                                    "complete in time — reset BS DTLS, retrying "
                                    "in %d s\n"),
                           *bootBackoff));
                *bootBackoff = (*bootBackoff * 2 > 30) ? 30 : (*bootBackoff * 2);
                *bootPhase = BootPhase::NotStarted;
            }

            // Register when ready: bootstrap committed + the DM DTLS session
            // is up, or bootstrap skipped (peer connected at startup).
            if ((*bootPhase == BootPhase::Done && dtlsReady) ||
                *bootPhase == BootPhase::Skipped) {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l sending "
                                    "Register\n")));
                auto payload = reg->build_register_request(
                    next_msgid(),
                    std::string{static_cast<char>(0x10)});
                tx_via(*a, payload, svc);
                // Endpoint change is naturally satisfied by this Register;
                // clear the flag so we don't double-act on it later.
                reg->clear_pending_reregister();
            }
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

        // L9 stub 2 — Update POST when the lifetime margin elapses. Defer it one
        // tick while a telemetry Send is in flight so the registration Update and
        // the Send never have overlapping 2.04 acks (the receive path can't tell
        // them apart). The Send acks within ack_timeout_s (6s) ≪ the 30s update
        // margin, so the keepalive is never starved.
        if (reg->should_send_update(now) &&
            !(sendst->up && sendst->up->in_flight())) {
            auto payload = reg->build_update_request(
                next_msgid(),
                std::string{static_cast<char>(0x02)},
                /*withAdvertisedSet*/ false);
            tx_via(*a, payload, svc);
            reg->note_update_sent(now);
            *lastKeepalive = now;     // the Update itself holds the NAT mapping
        }

        // NAT keepalive (apps/docs/keepalive note): a small CoAP ping (empty
        // CON) holds the UDP mapping when the Update cadence (lifetime − margin)
        // exceeds the link's NAT idle timeout. Only while cleanly Registered;
        // the Update above resets the timer so a ping never trails it.
        if (reg->keepalive_seconds() > 0 &&
            reg->state() == ::lwm2m::RegistrationState::Registered &&
            now - *lastKeepalive >=
                std::chrono::seconds(reg->keepalive_seconds())) {
            tx_via(*a,
                   ::lwm2m::RegistrationClient::build_keepalive_ping(next_msgid()),
                   svc);
            *lastKeepalive = now;
        }

        // ── v2 Send telemetry (TDD §3b #1). Only while cleanly Registered —
        // never mid-Update/Register/Deregister — so a Send's 2.04 can't be
        // mistaken for a registration ack. No-op unless enabled (sendst->up set).
        if (sendst->up &&
            reg->state() == ::lwm2m::RegistrationState::Registered) {
            auto& up = *sendst->up;
            // 1. sample numeric vehicle.* into the buffer at the cadence.
            if (now - sendst->last_sample >=
                std::chrono::seconds(sendst->sample_secs)) {
                sendst->last_sample = now;
                const double tnow = std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                ::lwm2m::telemetry::Sample s;
                if (build_vehicle_sample(ds.client(), tnow, s)) up.offer(s);
            }
            // 2. time out an unacked Send → requeue (retried on the next poll).
            if (up.in_flight() &&
                now - sendst->inflight_since >=
                    std::chrono::seconds(sendst->ack_timeout_s)) {
                up.on_timeout();
            }
            // 3. transmit the next batch (one CON /dp at a time) when idle.
            if (!up.in_flight() && up.pending() > 0) {
                auto wire = up.poll(next_msgid(),
                                    std::string{static_cast<char>(0x30)});
                if (!wire.empty()) {
                    tx_via(*a, wire, svc);
                    sendst->inflight_since = now;
                    *lastKeepalive = now;   // a Send also holds the NAT mapping
                }
            }
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
                // A prior successful bootstrap pinned the DM identity + pointed
                // the data plane at the DM. Re-point to the BS and force a fresh
                // BS DTLS handshake; reset_and_connect() restores the BS identity
                // so the /bs handshake offers BS creds, not the DM identity+key
                // the BS resolver can't match (else the device wedges in
                // bootstrapping after a cloud restart).
                for (auto& [type, ctx] : a->udpAdapter()->services()) {
                    if (type != ::UDPAdapter::ServiceType_t::LwM2MClient) continue;
                    ctx->peerHost(bsHost);
                    ctx->peerPort(bsPort);
                }
                if (dtls) dtls->reset_and_connect(bsHost, bsPort);
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
    if (auto* cli = ds.client()) {
        g_log.apply_level(*cli);
        dtls_apply_log_level(*cli);   // tinydtls level from log.level.dtls/log.level
        g_log.open(*cli, 5, 1);
    }

    // RPi serial auto-fill — MUST run BEFORE the provisioning park below.
    // The serial IS the endpoint + BS PSK identity, and the operator reads
    // it off the device-ui to generate the BS PSK and commission the device.
    // If this waited until after the park (which blocks on iot.bs.uri + the
    // BS PSK), iot.serial would still be empty exactly when the operator is
    // trying to read it — which is why the field showed blank and had to be
    // typed by hand. Resolve + persist it up front so it is live immediately.
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
            // BS PSK identity is normally DERIVED from the endpoint (sha256),
            // so only the secret (iot.bs.psk.key) is commissioned. With the
            // third-party override on, the operator-supplied identity must also
            // be present before we proceed.
            auto pkey = ds.bs_psk_key();
            const bool have_psk =
                (pkey && !pkey->empty()) ||
                (!argValueMap["identity"].empty() && !argValueMap["secret"].empty());
            bool have_identity = true;
            if (ds.bs_psk_override()) {
                auto bid = ds.bs_psk_identity();
                have_identity = bid && !bid->empty();
            }
            if (!bsUri.empty() && have_psk && have_identity) break;
            if (!logged_wait) {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l awaiting provisioning "
                                    "(iot.bs.uri + BS PSK via device-ui commissioning)\n")));
                logged_wait = true;
            }
            // Pet the systemd watchdog while we wait — the reactor that normally
            // pings it (run_reactor_event_loop) hasn't started yet, so without
            // this an un(der)-provisioned device (or one that briefly reads empty
            // creds during a ds-load race after a restart) is SIGABRT'd every
            // WatchdogSec (#406) → a 60s crash-loop that defeats this loop's
            // "stay alive until commissioned" intent.
            systemd_watchdog_ping();
            ACE_OS::sleep(ACE_Time_Value(5, 0));
        }
        UDPAdapter::Scheme_t bsScheme;
        parsePeerOption(bsUri, bsScheme, bsHost, bsPort);
    }

    // Endpoint (CLI ep= / data-store serial / RPi auto-detect). Declared here
    // because the BS PSK identity is sha256(endpoint) below.
    std::string endpoint = epres.ready ? epres.endpoint
                                       : std::string("urn:dev:client-1");

    // No hardcoded identity/secret. The client derives its BS DTLS identity
    // from the endpoint and reads the secret from ds; the server
    // authenticates against per-endpoint PSKs loaded from
    // cloud.endpoint.credentials (register_endpoint_creds below). Both are
    // generated → stored in ds → read from ds — nothing is baked in.
    std::string identity, secret;
    if(scheme == UDPAdapter::Scheme_t::CoAPs) {
        auto dsKey = ds.bs_psk_key();
        if (UDPAdapter::Role_t::CLIENT == role) {
            // BS DTLS PSK identity. Two modes:
            //   * default — DERIVED from the endpoint: both the device and the
            //     cloud BS compute identity = sha256(endpoint), so it is never
            //     stored/commissioned. 128-bit identity (first 32 hex chars of
            //     sha256) — same size as the 128-bit BS PSK, and fits tinydtls'
            //     32-byte identity buffer.
            //   * override (iot.bs.psk.override=true) — the operator pinned a
            //     custom identity in iot.bs.psk.identity for a third-party
            //     bootstrap server that mints its own identity/PSK pair; use it
            //     VERBATIM. The DM credential path is unaffected either way.
            if (ds.bs_psk_override()) {
                auto bsId = ds.bs_psk_identity();
                if (bsId && !bsId->empty()) {
                    identity = *bsId;
                } else {
                    ACE_ERROR((LM_ERROR,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK override on "
                                        "but iot.bs.psk.identity empty — provision it via "
                                        "device-ui\n")));
                    return(-2);
                }
            } else {
                identity = iot::sha256_hex(endpoint).substr(0, 32);
            }
            if (dsKey && !dsKey->empty())            secret = *dsKey;
            else if (!argValueMap["secret"].empty()) secret = argValueMap["secret"];
            else {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK secret missing "
                                    "(provision iot.bs.psk.key or pass secret=)\n")));
                return(-2);
            }
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK identity=%C (%C)\n"),
                       identity.c_str(),
                       ds.bs_psk_override() ? "custom override" : "sha256(endpoint)"));
        } else {
            // SERVER role: per-endpoint PSKs come from cloud.endpoint.credentials
            // (registered below). An explicit shared identity/secret may still
            // be supplied via CLI for a dev/test harness, but it is NOT required
            // and NOT hardcoded — a provisioned fleet needs none.
            if (!argValueMap["identity"].empty() &&
                !argValueMap["secret"].empty()) {
                identity.assign(argValueMap["identity"]);
                secret.assign(argValueMap["secret"]);
            }
        }
    }

    // Service type is selected from the launch args: client → LwM2MClient;
    // server → BS or DM per `lwm2m-instance` (the cloud runs the SAME binary
    // twice, lwm2m-instance=bs on 5684 and =dm on 5683). Each server instance
    // then binds ONLY its own `local` port. The old code typed every server as
    // BootstrapServer and additionally hardcoded init(5683, DeviceMgmtServer),
    // which double-bound 5683 in the dm instance (benign EADDRINUSE) and left an
    // unused 5683 listener in the bs instance. Both handlers (bsServer +
    // regServer) are attached to EVERY context below and processRequest routes
    // /bs and /rd by URI, so one bound socket per instance serves both.
    UDPAdapter::ServiceType_t service;
    if(UDPAdapter::Role_t::CLIENT == role) {
        service = UDPAdapter::ServiceType_t::LwM2MClient;
    } else if(argValueMap["lwm2m-instance"] == "dm") {
        service = UDPAdapter::ServiceType_t::DeviceMgmtServer;
    } else {
        service = UDPAdapter::ServiceType_t::BootsstrapServer;
    }

    std::shared_ptr<App> app = std::make_shared<App>(selfHost, selfPort, scheme, service);
    app->udpAdapter()->add_event_handle(scheme, service);

    if(UDPAdapter::Role_t::CLIENT == role) {
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

    // Server role: resolve per-device BS/DM PSKs LIVE from the cloud's
    // cloud.endpoint.credentials array for the identity the client presents at
    // the DTLS handshake — no startup pre-load and no watch. The presented id
    // is BS = sha256(serial)[:32] (exactly what the device derives from its
    // endpoint) or DM = dm.psk.id. The resolver runs on the handshake/reactor
    // thread (NOT the ds listener thread), so the blocking ds get() is safe;
    // this mirrors the BS provisioning_resolver that already reads ds per /bs.
    // Reading the array requires gid:cloud-svc (the servers run as cloud-svc).
    // Net effect: a device provisioned after the server started authenticates
    // immediately, and ds stays the single source of truth.
    auto install_psk_resolver = [&](auto dtls) {   // dtls: shared_ptr<DTLSAdapter>
        if (!dtls || UDPAdapter::Role_t::SERVER != role || !ds.client()) return;
        const bool is_bs = argValueMap["lwm2m-instance"] == "bs";
        auto* cli = ds.client();
        // KEK for the zero-touch HKDF tier — read once, captured by value. Empty
        // ⇒ tier disabled (commissioned per-device lookup still serves).
        const std::string bsKekHex = load_bs_master_kek_hex();
        dtls->set_psk_resolver([cli, is_bs, bsKekHex](const std::string& presented) -> std::string {
            std::vector<data_store::Client::GetResult> got;
            auto rs = cli->get({std::string("cloud.endpoint.credentials"),
                                std::string("cloud.bs.master.key")}, got);
            if (!rs.ok || got.empty()) return std::string();
            const std::string credsJson =
                got[0].has_value ? data_store::to_string(got[0].value).value_or("")
                                 : std::string();
            // Unwrap the HKDF master if a KEK is configured (bad/tampered → "").
            const std::string wrapped =
                (got.size() > 1 && got[1].has_value)
                    ? data_store::to_string(got[1].value).value_or("")
                    : std::string();
            const std::string masterHex =
                bsKekHex.empty()
                    ? std::string()
                    : iot::unwrap_bs_master_hex(bsKekHex, wrapped).value_or("");
            // BS tier: commissioned sha256(serial)[:32] lookup → else derive from
            // the presented raw serial. DM tier: dm.psk.id lookup → else derive
            // from the serial embedded in the presented identity.
            const std::string key =
                is_bs ? iot::resolve_bs_psk(credsJson, presented, masterHex)
                      : iot::resolve_dm_psk(credsJson, presented, masterHex);
            // Log the MISS with the presented identity — tinydtls only says
            // "PSK for unknown identity requested" without it, which makes a
            // provisioning gap (no matching cloud.endpoint.credentials row, and
            // no HKDF master to derive) impossible to debug from the cloud log.
            if (key.empty()) {
                ACE_ERROR((LM_WARNING,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l PSK resolver: no "
                                    "key for %C identity '%C' — not in "
                                    "cloud.endpoint.credentials%C\n"),
                           is_bs ? "BS" : "DM", presented.c_str(),
                           masterHex.empty()
                               ? " (HKDF tier off)"
                               : " and HKDF derive declined"));
            }
            return key;
        });
    };

    if(UDPAdapter::Scheme_t::CoAPs == scheme) {
        auto it = std::find_if(app->udpAdapter()->services().begin(), app->udpAdapter()->services().end(),[&](auto& ent) -> bool {
            return(service == ent.second->service());
        });

        if(it != app->udpAdapter()->services().end()) {
            auto& ent = *it;
            // Only register a shared identity/secret when one was explicitly
            // provided (client: derived id + ds secret; dev server override).
            // Empty → server relies solely on the ds-backed resolver below.
            if (!identity.empty() && !secret.empty())
                ent.second->dtlsAdapter()->add_credential(identity, secret);
            install_psk_resolver(ent.second->dtlsAdapter());
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
    // (endpoint resolved above, before the BS PSK identity block.)

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
        dtls_apply_log_level(*cli);
        g_log.flush(*cli);  // push startup logs immediately

        // Hot-reload: the lwm2m binary runs its event loop inside the ACE
        // reactor (UDPAdapter::start) and has no pull-style watch loop like
        // cloudd/httpd, so without this a log.level change needed a restart.
        // A callback-style watch fires on the ds notification thread; re-apply
        // the ACE mask (bumps the generation → the reactor thread re-pins via
        // refresh_level()) and the tinydtls level. Watch the global key, this
        // instance's per-daemon key, and the DTLS key.
        const std::string lvl_key = lwm2m_instance.empty()
            ? std::string("log.level.lwm2m")
            : ("log.level.lwm2m." + lwm2m_instance);
        static data_store::Client::WatchHandle s_lvl_wh =
            data_store::Client::kInvalidHandle;
        cli->watch(
            std::vector<std::string>{"log.level", lvl_key, "log.level.dtls"},
            [&ds](const data_store::Client::Event&) {
                if (auto* c = ds.client()) {
                    g_log.apply_level(*c);
                    dtls_apply_log_level(*c);
                }
            },
            &s_lvl_wh);
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

        // The net.router dependency + service-enable gate is a DEVICE concern
        // (the gateway's routing must be up first). The cloud BS/DM
        // (lwm2m-instance set) are always-on, docker-compose-managed, and have
        // no net-router — so skip the gate entirely. Otherwise they park on
        // net.router forever and their reactor never reads the DTLS socket
        // (so no client can ever bootstrap/register).
        if (lwm2m_instance.empty()) {
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
    }

    ClientPlumbing clientPlumbing;
    ServerPlumbing serverPlumbing;
    if (UDPAdapter::CLIENT == role) {
        clientPlumbing = wire_client(app, endpoint, configDir, bsHost, bsPort, ds);
    } else {
        serverPlumbing = wire_server(app, ds, lwm2m_instance);
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
        ds.on_change([wreg, rebind, &ds, started_bs_psk = secret,
                      started_bs_override = ds.bs_psk_override(),
                      started_bs_identity = ds.bs_psk_identity().value_or(std::string())]
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
            // Third-party BS override / custom identity change: the on-the-wire
            // BS identity changes when the override flag flips (derived
            // sha256(endpoint) <-> custom) or when the custom identity is edited
            // while override is on. Either way the next bootstrap DTLS handshake
            // needs the new identity, so self-exit for a clean systemd restart —
            // same policy as the BS PSK key above, yielding a fresh DTLS session.
            // A bare iot.bs.psk.identity write while override is OFF is a no-op
            // for the handshake (the derived identity is used), so it must NOT
            // churn the process — e.g. the RPi serial auto-fill writes that key
            // on every boot.
            if (k == iot::DsConfig::Key::BsPskOverride ||
                k == iot::DsConfig::Key::BsPskIdentity) {
                const bool override_now = ds.bs_psk_override();
                const std::string id_now = ds.bs_psk_identity().value_or("");
                const bool override_changed = (override_now != started_bs_override);
                const bool identity_changed =
                    override_now && (id_now != started_bs_identity);
                if (override_changed || identity_changed) {
                    ACE_ERROR((LM_WARNING,
                               ACE_TEXT("%D lwm2m:thread:%t %M %N:%l BS PSK "
                                        "identity/override changed — exiting for "
                                        "systemd restart with new credentials\n")));
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