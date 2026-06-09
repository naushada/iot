/// iot-cloudd — Cloud server daemon (L21/D8).
///
/// Wires cloud-specific modules that mirror the device-side stack:
///
///   Device                Cloud
///   ──────                ─────
///   data-store (ds)    →  ds-server (same binary, cloud.* keys)
///   openvpn-client      →  openvpn-server (managed by VPN registry)
///   net-router           →  nftables per-device DNAT (port 5000+ → tun IP)
///   iot-httpd            →  iot-httpd + server/web proxy (device UI proxy)
///   lwm2m client         →  lwm2m server (CoAP /bs, /push, /rd)
///
/// IPC: ALL daemons use data_store::Client → ds-server.
/// No HTTP between daemons — same pattern as device side.

#include "endpoint_registry.hpp"
#include "vpn_registry.hpp"
#include "openvpn_server.hpp"
#include "cert_authority.hpp"
#include "bootstrap.hpp"
#include "cloud_credentials.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <ace/Log_Msg.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"

#include <atomic>
#include <cstdint>
#include <csignal>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace {

// ── Log ring buffer ────────────────────────────────────────────────
// Captures ACE log output → ds log.cloudd.text for the cloud UI.
data_store::LogBuffer g_log("cloudd", "log.cloudd.text", "log.level.cloudd");

std::atomic<bool> g_stop{false};

// Current openvpn(8) server pid (0 = not running). Written only by the
// main thread's supervisor; read by the per-pid StatsPublisher thread.
std::atomic<long> g_ovpn_pid{0};

void on_signal(int) { g_stop.store(true); }

// Read a string ds key with a fallback default.
std::string ds_str(data_store::Client& ds, const std::string& key,
                   const std::string& dflt) {
    std::vector<data_store::Client::GetResult> got;
    auto s = ds.get({key}, got);
    if (s.ok && !got.empty() && got[0].has_value)
        if (auto v = data_store::to_string(got[0].value)) return *v;
    return dflt;
}

// Read an integer ds key with a fallback default.
int ds_int(data_store::Client& ds, const std::string& key, int dflt) {
    std::vector<data_store::Client::GetResult> got;
    auto s = ds.get({key}, got);
    if (s.ok && !got.empty() && got[0].has_value)
        if (auto v = data_store::to_int32(got[0].value)) return *v;
    return dflt;
}

// Sync endpoint registry to cloud.endpoints JSON in the data store.
void sync_endpoints_to_ds(data_store::Client& ds,
                           server::lwm2m::EndpointRegistry& reg) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ep : reg.list_all()) {
        nlohmann::json item;
        item["endpoint"]      = ep.ep;
        item["state"]         = ep.registered ? "online" : "offline";
        item["tun_ip"]        = ep.tun_ip;
        item["proxy_port"]    = ep.proxy_port;
        item["registered"]    = ep.registered;
        item["last_seen_unix"] = ep.last_seen_unix;
        arr.push_back(item);
    }
    ds.set("cloud.endpoints", data_store::Value{arr.dump()});
}

// Merge lwm2m-dm's registration status into the EndpointRegistry. lwm2m-dm
// writes cloud.lwm2m.registrations = [{ endpoint, registered, last_seen_unix }]
// (the set of currently-registered endpoints); we flip each provisioned
// endpoint's `registered` flag to match so sync_endpoints_to_ds reports real
// online/offline. Endpoints absent from the list are offline. Returns true if
// any endpoint's state changed.
bool reconcile_registrations(data_store::Client& ds,
                             server::lwm2m::EndpointRegistry& reg) {
    const std::string js = ds_str(ds, "cloud.lwm2m.registrations", "[]");
    std::unordered_map<std::string, std::int64_t> online;  // ep → last_seen_unix
    try {
        auto arr = nlohmann::json::parse(js);
        if (arr.is_array()) {
            for (const auto& e : arr) {
                if (!e.is_object()) continue;
                if (!e.value("registered", false)) continue;
                auto ep = e.value("endpoint", std::string());
                if (ep.empty()) continue;
                online[std::move(ep)] = e.value("last_seen_unix",
                                                std::int64_t{0});
            }
        }
    } catch (const std::exception& ex) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                            "cloud.lwm2m.registrations parse: %C\n"),
                   ex.what()));
        return false;
    }
    bool changed = false;
    for (const auto& ep : reg.list_all()) {
        auto it = online.find(ep.ep);
        if (it != online.end()) {
            // Online — refresh the registered flag + last-seen timestamp.
            reg.update_state(ep.ep, true, it->second);
            if (!ep.registered || ep.last_seen_unix != it->second)
                changed = true;
        } else if (ep.registered) {
            // Offline — clear the flag but keep the last-seen value.
            reg.update_state(ep.ep, false);
            changed = true;
        }
    }
    return changed;
}

} // namespace

int main(int argc, char** argv) {
    // ── CLI args ──────────────────────────────────────────────────
    std::string ds_path = "/var/run/iot/data_store.sock";
    std::string vpn_subnet = "10.9.0.0/24";
    int proxy_port_start = 5001;
    int proxy_port_end   = 6000;
    int sync_interval    = 10;  // seconds

    for (int i = 1; i < argc; ++i) {
        std::string a{argv[i]};
        if (a.rfind("ds-socket=", 0) == 0)
            ds_path = a.substr(10);
        else if (a.rfind("vpn-subnet=", 0) == 0)
            vpn_subnet = a.substr(11);
        else if (a.rfind("proxy-start=", 0) == 0)
            proxy_port_start = std::stoi(a.substr(12));   // "proxy-start=" = 12 chars
        else if (a.rfind("proxy-end=", 0) == 0)
            proxy_port_end = std::stoi(a.substr(10));      // "proxy-end=" = 10 chars
        else if (a.rfind("sync-interval=", 0) == 0)
            sync_interval = std::stoi(a.substr(14));
    }

    g_log.start();

    // ── Connect to ds-server ──────────────────────────────────────

    // ── Connect to ds-server ──────────────────────────────────────
    data_store::Client ds;
    auto cs = ds.connect(ds_path);
    if (!cs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l ds-server connect failed: %C\n"),
                   cs.err.c_str()));
        return 1;
    }

    // ── Core services ─────────────────────────────────────────────
    server::openvpn::VpnRegistry vpn_reg(vpn_subnet,
        static_cast<std::uint16_t>(proxy_port_start),
        static_cast<std::uint16_t>(proxy_port_end));
    server::lwm2m::EndpointRegistry ep_reg;
    server::lwm2m::BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    // ── Watch for provision / deprovision requests ────────────────
    // iot-httpd writes cloud.provision.request / cloud.deprovision.request
    // to the data store; we pull-style watch both keys and act on changes.
    // Events arrive via recv_event() in the main loop below.
    auto ws_prov = ds.watch("cloud.provision.request", 1000);
    if (!ws_prov.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l watch provision.request failed: %C\n"),
                   ws_prov.err.c_str()));
    }
    auto ws_depr = ds.watch("cloud.deprovision.request", 1000);
    if (!ws_depr.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l watch deprovision.request failed: %C\n"),
                   ws_depr.err.c_str()));
    }
    // Watch log.level so operator changes via cloud UI take effect
    // immediately (no need to wait for the periodic timeout tick).
    auto ws_loglevel = ds.watch("log.level", 5000);
    if (!ws_loglevel.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l watch log.level failed: %C\n"),
                   ws_loglevel.err.c_str()));
    }
    // Watch lwm2m-dm's registration status so online/offline transitions
    // reach cloud.endpoints promptly (the periodic tick is a safety net).
    auto ws_regs = ds.watch("cloud.lwm2m.registrations", 1000);
    if (!ws_regs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l watch "
                            "cloud.lwm2m.registrations failed: %C\n"),
                   ws_regs.err.c_str()));
    }

    // Seed initial VPN config in ds
    ds.set("cloud.vpn.subnet", data_store::Value{vpn_subnet});
    ds.set("cloud.vpn.port.next",
           data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});

    // Self-report running state to ds for Services page.
    // openvpn-server is managed internally by iot-cloudd — report its state too.
    {
        auto rs = ds.set("services.cloud.iot.cloudd.state",
                         data_store::Value{std::string("running")});
        if (!rs.ok) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l set cloudd.state=running"
                                " failed: %C\n"),
                       rs.err.c_str()));
        }
    }
    // ── OpenVPN server — spawn + supervise (config from cloud.vpn.*) ──
    server::openvpn::OpenVpnServerConfig ovpn_cfg;
    ovpn_cfg.subnet    = ds_str(ds, "cloud.vpn.subnet",     "10.9.0.0/24");
    ovpn_cfg.proto     = ds_str(ds, "cloud.vpn.proto",      "tcp-server");
    ovpn_cfg.ca_path   = ds_str(ds, "cloud.vpn.ca.crt",     "/etc/iot/vpn/ca/ca.crt");
    ovpn_cfg.cert_path = ds_str(ds, "cloud.vpn.server.crt", "/etc/iot/vpn/server.crt");
    ovpn_cfg.key_path  = ds_str(ds, "cloud.vpn.server.key", "/etc/iot/vpn/server.key");
    ovpn_cfg.cipher    = ds_str(ds, "cloud.vpn.cipher",     "AES-256-GCM");
    ovpn_cfg.dev       = ds_str(ds, "cloud.vpn.dev",        "tun");
    ovpn_cfg.port      = static_cast<std::uint16_t>(ds_int(ds, "cloud.vpn.listen.port", 1194));
    ovpn_cfg.mgmt_port = static_cast<std::uint16_t>(ds_int(ds, "cloud.vpn.mgmt.port", 7506));
    ovpn_cfg.verb      = ds_int(ds, "cloud.vpn.verb", 3);
    // Seed the effective config back to ds so every cloud.vpn.* key exists
    // with its default (visible in the cloud UI / ds-cli).
    ds.set({
        {"cloud.vpn.proto",       data_store::Value{ovpn_cfg.proto}},
        {"cloud.vpn.cipher",      data_store::Value{ovpn_cfg.cipher}},
        {"cloud.vpn.dev",         data_store::Value{ovpn_cfg.dev}},
        {"cloud.vpn.ca.crt",      data_store::Value{ovpn_cfg.ca_path}},
        {"cloud.vpn.server.crt",  data_store::Value{ovpn_cfg.cert_path}},
        {"cloud.vpn.server.key",  data_store::Value{ovpn_cfg.key_path}},
        {"cloud.vpn.listen.port", data_store::Value{static_cast<std::int32_t>(ovpn_cfg.port)}},
        {"cloud.vpn.mgmt.port",   data_store::Value{static_cast<std::int32_t>(ovpn_cfg.mgmt_port)}},
        {"cloud.vpn.verb",        data_store::Value{static_cast<std::int32_t>(ovpn_cfg.verb)}},
    });
    // ── Runtime VPN PKI (Phase 2) ─────────────────────────────────
    // The cloud image generates a CA + server cert at build time but PURGES
    // the CA key, so the shipped image can't sign new client certs. Generate
    // and persist a runtime CA (+ a server cert signed by it) before openvpn
    // starts; the same CA signs the per-device client certs minted at
    // provision time, so the server trusts them.
    server::openvpn::CaPaths capaths;
    capaths.ca_crt  = ovpn_cfg.ca_path;
    capaths.srv_crt = ovpn_cfg.cert_path;
    capaths.srv_key = ovpn_cfg.key_path;
    {
        auto slash = ovpn_cfg.ca_path.find_last_of('/');
        capaths.ca_key = (slash == std::string::npos)
                       ? std::string("ca.key")
                       : ovpn_cfg.ca_path.substr(0, slash + 1) + "ca.key";
    }
    server::openvpn::CertAuthority cert_ca(capaths);
    if (!cert_ca.ensure()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l runtime CA ensure() "
                            "failed — per-device client certs won't be mintable\n")));
    }

    server::openvpn::OpenVpnServer ovpn(ovpn_cfg);

    // Bring openvpn to the operator-desired state and publish it. Runs on
    // the main thread only (single owner of the child process / waitpid).
    auto supervise_ovpn = [&ds, &ovpn]() {
        bool enabled = true;
        {
            std::vector<data_store::Client::GetResult> got;
            if (ds.get({"services.cloud.openvpn.server.enable"}, got).ok &&
                !got.empty() && got[0].has_value)
                if (auto b = data_store::to_bool(got[0].value)) enabled = *b;
        }
        std::string state;
        if (enabled) {
            const bool was = ovpn.running();
            if (!was) {
                if (ovpn.start()) {
                    ACE_DEBUG((LM_INFO, ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                               "openvpn server (re)started\n")));
                }
            }
            state = ovpn.running() ? "running" : "exited";
        } else {
            if (ovpn.running()) ovpn.stop();
            state = "disabled";
        }
        g_ovpn_pid.store(ovpn.running() ? ovpn.pid() : 0);
        ds.set("services.cloud.openvpn.server.state",
               data_store::Value{state});
    };
    supervise_ovpn();   // initial start

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l started, ds=%C vpn-subnet=%C"
                        " proxy=%d-%d sync-interval=%d\n"),
               ds_path.c_str(), vpn_subnet.c_str(),
               proxy_port_start, proxy_port_end, sync_interval));

    // ── Log level from data store (reused from device pattern) ─────
    // Lambda applies the log.level string to ACE_Log_Msg::priority_mask.
    // Called at startup, on watch events, and on periodic timeout ticks.
    g_log.apply_level(ds);
    g_log.flush(ds);  // push startup logs immediately

    // ── Resource telemetry (L22) ──────────────────────────────────
    // Publish this container's CPU/mem/fd/threads to ds every 10s for the
    // cloud UI Services page. openvpn-server runs inside this same
    // container, so its usage is already folded into cloudd's cgroup
    // totals — we publish under services.cloud.iot.cloudd only.
    data_store::StatsPublisher g_stats(
        "services.cloud.iot.cloudd",
        [&ds](const std::vector<data_store::KV>& kv) { ds.set(kv); });
    if (g_stats.open() != 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l stats publisher "
                            "open failed\n")));
    }

    // Flush logs to ds every 10s via LogBuffer's own ACE reactor timer
    // (same pattern as StatsPublisher) instead of from the loop below.
    g_log.open(ds, 10, 200);

    // Per-PID telemetry for the openvpn child (own reactor thread; reads
    // only the atomic pid the main-thread supervisor maintains).
    data_store::StatsPublisher g_ovpn_stats(
        "services.cloud.openvpn.server",
        [&ds](const std::vector<data_store::KV>& kv) { ds.set(kv); },
        []() -> long { return g_ovpn_pid.load(); });
    g_ovpn_stats.open();

    // ── Main loop ─────────────────────────────────────────────────
    // Block on recv_event() up to sync_interval seconds.  A provision
    // or deprovision request from iot-httpd wakes us immediately; a
    // timeout triggers the periodic endpoint sync to keep ds fresh.
    int sync_tick = 0;
    while (!g_stop.load()) {
        data_store::Client::Event ev;
        auto rs = ds.recv_event(ev, sync_interval * 1000);

        if (rs.ok) {
            // A watched key changed
            if (ev.key == "cloud.provision.request") {
                if (auto ep = data_store::to_string(ev.value)) {
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D cloudd:thread:%t %M %N:%l provision request '%C'\n"),
                               ep->c_str()));

                    // PSK provisioning (task M-wire): the endpoint name IS
                    // the raw serial. Read the engineer-pasted BS PSK from
                    // the carrier, mint a DM PSK, and upsert a per-endpoint
                    // credential record keyed by rpi<serial>@cloud.local.
                    // Clear the carrier afterwards (it held a secret).
                    const std::string bs_psk =
                        ds_str(ds, "cloud.provision.bs.psk", "");
                    if (!bs_psk.empty()) {
                        try {
                            // 128-bit (16-byte) DM PSK. tinydtls'
                            // TLS_PSK_WITH_AES_128_CCM_8 key buffer is 16 bytes,
                            // so a 32-byte PSK overflows it ("PSK exceeds caller
                            // buffer") and the DM handshake fails. Matches the
                            // 128-bit BS PSK.
                            const std::string dm_psk =
                                server::lwm2m::generate_psk_hex(16);
                            const std::string cur =
                                ds_str(ds, "cloud.endpoint.credentials", "[]");
                            const std::string next =
                                server::lwm2m::upsert_credential(
                                    cur, *ep, bs_psk, dm_psk);
                            ds.set("cloud.endpoint.credentials",
                                   data_store::Value{next});
                            ds.set("cloud.provision.bs.psk",
                                   data_store::Value{std::string("")});
                            ACE_DEBUG((LM_INFO,
                                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l stored "
                                                "credentials for %C (identity=%C)\n"),
                                       ep->c_str(),
                                       server::lwm2m::format_identity(*ep).c_str()));
                        } catch (const std::exception& e) {
                            ACE_ERROR((LM_ERROR,
                                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l credential "
                                                "provisioning for %C failed: %C\n"),
                                       ep->c_str(), e.what()));
                        }
                    }

                    // Mint a per-device VPN client cert (Phase 2) signed by
                    // the runtime CA and stash it in the credential record for
                    // LwM2M Object-2048 delivery (Phase 3). CN = the cloud
                    // formatted identity, for traceability.
                    if (cert_ca.have_ca()) {
                        const std::string cn =
                            server::lwm2m::format_identity(*ep);
                        if (auto mc = cert_ca.mint_client(cn)) {
                            try {
                                const std::string cur =
                                    ds_str(ds, "cloud.endpoint.credentials", "[]");
                                const std::string next =
                                    server::lwm2m::upsert_vpn_cert(
                                        cur, *ep, mc->client_crt, mc->client_key);
                                ds.set("cloud.endpoint.credentials",
                                       data_store::Value{next});
                                ACE_DEBUG((LM_INFO,
                                           ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                                                    "minted+stored VPN client cert "
                                                    "for %C\n"), ep->c_str()));
                            } catch (const std::exception& e) {
                                ACE_ERROR((LM_ERROR,
                                           ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                                                    "store VPN cert for %C failed: "
                                                    "%C\n"), ep->c_str(), e.what()));
                            }
                        }
                    }

                    auto result = provisioner.provision(*ep);
                    if (result.has_value()) {
                        ACE_DEBUG((LM_INFO,
                                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l provisioned %C"
                                            " tun=%C proxy=%d\n"),
                                   result->endpoint.c_str(), result->tun_ip.c_str(),
                                   static_cast<int>(result->proxy_port)));
                    } else {
                        ACE_ERROR((LM_ERROR,
                                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l provision failed"
                                            " '%C' (dup or exhausted)\n"),
                                   ep->c_str()));
                    }
                }
            } else if (ev.key == "cloud.deprovision.request") {
                if (auto ep = data_store::to_string(ev.value)) {
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D cloudd:thread:%t %M %N:%l deprovision request '%C'\n"),
                               ep->c_str()));
                    bool ok = provisioner.deprovision(*ep);
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D cloudd:thread:%t %M %N:%l deprovision %C '%C'\n"),
                               (ok ? "ok" : "failed (not found)"), ep->c_str()));
                }
            } else if (ev.key == "log.level") {
                g_log.apply_level(ds);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D cloudd:thread:%t %M %N:%l log level changed\n")));
            } else if (ev.key == "cloud.lwm2m.registrations") {
                // lwm2m-dm published a registration change — fold online/
                // offline into the endpoint registry before the sync below.
                reconcile_registrations(ds, ep_reg);
            }
            // Sync after every event so the UI sees changes quickly
            sync_endpoints_to_ds(ds, ep_reg);
            sync_tick = 0;
        } else {
            // recv_event timed out — periodic housekeeping
            supervise_ovpn();   // restart on crash / honor enable flips
            if (++sync_tick >= 3) {
                sync_tick = 0;
                // Safety-net reconcile in case a watch notification was missed.
                reconcile_registrations(ds, ep_reg);
                sync_endpoints_to_ds(ds, ep_reg);
                ds.set("cloud.vpn.port.next",
                       data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});
                g_log.apply_level(ds);
            }
        }
    }

    {
        auto rs = ds.set("services.cloud.iot.cloudd.state",
                         data_store::Value{std::string("exited")});
        if (!rs.ok) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l set cloudd.state=exited"
                                " failed: %C\n"),
                       rs.err.c_str()));
        }
    }
    g_ovpn_stats.close();
    ovpn.stop();
    g_ovpn_pid.store(0);
    ds.set("services.cloud.openvpn.server.state",
           data_store::Value{std::string("exited")});
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l stopped\n")));
    g_log.close();   // stop flush timer + final flush (ds still alive)
    return 0;
}
