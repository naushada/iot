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
#include "device_dnat.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <ace/Log_Msg.h>
#include <ace/INET_Addr.h>
#include <ace/SOCK_Connector.h>
#include <ace/SOCK_Stream.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <fstream>
#include <set>
#include <thread>
#include <unordered_map>

#include <sys/stat.h>

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

// Read a whole file into a string ("" if absent/unreadable).
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Write a string to a file (parent dir created), with the given mode.
bool write_file(const std::string& path, const std::string& body, mode_t mode) {
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos) ::mkdir(path.substr(0, slash).c_str(), 0755);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    f.close();
    ::chmod(path.c_str(), mode);
    return static_cast<bool>(f);
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

// Rebuild + apply the per-device "device UI over VPN" DNAT ruleset.
//
// The device list is read from the *persisted* cloud.endpoints ds JSON (not
// the in-memory EndpointRegistry, which is empty until devices re-provision),
// so a fresh iot-cloudd reconstructs the full rule set on startup. nft -f -
// is atomic and our scoped table is flushed first, so this is idempotent +
// rebuildable: one call after each sync (and at startup) restores every
// device's mapping. ui_port is read live from cloud.proxy.device.ui.port
// (default 80 — the device UI is on 80/443).
//
// A device gets a rule once it has both a tun_ip and a proxy_port (i.e. it
// has been provisioned); online/offline state doesn't gate it (the connection
// simply fails if the tunnel is down, like any port-forward).
void rebuild_device_dnat(data_store::Client& ds, const std::string& tun_dev) {
    server::dnat::RulesetInput in;
    in.tun_dev = tun_dev;
    in.ui_port = static_cast<std::uint16_t>(
        ds_int(ds, "cloud.proxy.device.ui.port", 80));
    try {
        auto arr = nlohmann::json::parse(ds_str(ds, "cloud.endpoints", "[]"));
        if (arr.is_array()) for (const auto& e : arr) {
            if (!e.is_object()) continue;
            std::string ip   = e.value("tun_ip", std::string());
            int         port = e.value("proxy_port", 0);
            if (ip.empty() || port <= 0) continue;
            in.devices.push_back({std::move(ip),
                                  static_cast<std::uint16_t>(port)});
        }
    } catch (const std::exception& ex) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l device-UI DNAT: "
                            "cloud.endpoints parse: %C\n"), ex.what()));
        return;
    }
    const std::string script = server::dnat::build_device_dnat_ruleset(in);
    if (server::dnat::apply_ruleset(script)) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l device-UI DNAT applied "
                            "(%u rule(s), ui_port=%d)\n"),
                   static_cast<unsigned>(in.devices.size()),
                   static_cast<int>(in.ui_port)));
    }
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

// Poll the openvpn server's management interface (127.0.0.1:mgmt_port) for
// connected clients and return their device serials. The minted client CN is
// rpi<serial>@cloud.local, so a connected client's CN appears in the `status`
// output; we scan for that pattern and extract the serial. Best-effort —
// returns empty on any connect/IO failure (e.g. openvpn not up yet).
std::vector<std::string> poll_vpn_connected(std::uint16_t mgmt_port) {
    std::vector<std::string> out;
    ACE_INET_Addr addr(mgmt_port, "127.0.0.1");
    ACE_SOCK_Connector conn;
    ACE_SOCK_Stream stream;
    ACE_Time_Value to(1, 0);
    if (conn.connect(stream, addr, &to) != 0) return out;
    const char cmd[] = "status\n";
    stream.send_n(cmd, sizeof(cmd) - 1);
    std::string resp;
    char buf[2048];
    for (;;) {
        ACE_Time_Value rto(1, 0);
        ssize_t n = stream.recv(buf, sizeof(buf), &rto);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
        if (resp.find("\nEND") != std::string::npos || resp.size() > 65536) break;
    }
    stream.close();

    const std::string suffix = "@cloud.local";
    std::set<std::string> seen;
    for (std::size_t pos = 0; (pos = resp.find("rpi", pos)) != std::string::npos; ) {
        std::size_t at = resp.find(suffix, pos);
        if (at == std::string::npos) break;
        std::string serial = resp.substr(pos + 3, at - (pos + 3));
        pos = at + suffix.size();
        if (!serial.empty() &&
            serial.find_first_of(",\n\r ") == std::string::npos &&
            seen.insert(serial).second) {
            out.push_back(std::move(serial));
        }
    }
    return out;
}

// Rehydrate the in-memory EndpointRegistry + VpnRegistry from persisted ds at
// startup. Both are in-memory only; without this they come up EMPTY after a
// restart/upgrade, and the main loop's first sync_endpoints_to_ds() then
// clobbers the persisted cloud.endpoints to [] — so a provisioned device
// "disappears" from the table on every cloud update and loses its tun_ip /
// proxy_port (→ no DNAT, no VPN cert push). We:
//   1) exact-restore tun_ip/proxy_port/registered from the last persisted
//      cloud.endpoints (stable ports across restarts), and
//   2) heal any provisioned endpoint (cloud.endpoint.credentials) that lacks a
//      registry row — e.g. a cloud.endpoints already clobbered to [] by the
//      pre-fix bug — by allocating a fresh tun_ip/proxy_port.
// PSKs are never touched: provision() only assigns tun_ip/port + registry row.
std::size_t rehydrate_registry(data_store::Client& ds,
                               server::lwm2m::EndpointRegistry& reg,
                               server::openvpn::VpnRegistry& vpn,
                               server::lwm2m::BootstrapProvisioner& prov) {
    std::size_t restored = 0, healed = 0;

    try {
        auto eps = nlohmann::json::parse(ds_str(ds, "cloud.endpoints", "[]"));
        if (eps.is_array()) {
            for (const auto& e : eps) {
                if (!e.is_object()) continue;
                auto ep = e.value("endpoint", std::string());
                if (ep.empty()) continue;
                server::lwm2m::EndpointInfo info(
                    ep, e.value("tun_ip", std::string()),
                    static_cast<std::uint16_t>(e.value("proxy_port", 0)),
                    e.value("registered", false));
                info.last_seen_unix = e.value("last_seen_unix", std::int64_t{0});
                if (reg.add(info)) {
                    vpn.reserve(info.ep, info.tun_ip, info.proxy_port);
                    ++restored;
                }
            }
        }
    } catch (const std::exception& ex) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D cloudd:thread:%t %M %N:%l rehydrate "
                   "cloud.endpoints parse: %C\n"), ex.what()));
    }

    try {
        auto creds = nlohmann::json::parse(
            ds_str(ds, "cloud.endpoint.credentials", "[]"));
        if (creds.is_array()) {
            for (const auto& c : creds) {
                if (!c.is_object()) continue;
                auto serial = c.value("serial", std::string());
                if (serial.empty() || reg.lookup_by_ep(serial)) continue;
                if (prov.provision(serial)) ++healed;
            }
        }
    } catch (const std::exception& ex) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D cloudd:thread:%t %M %N:%l rehydrate "
                   "cloud.endpoint.credentials parse: %C\n"), ex.what()));
    }

    if (restored || healed)
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D cloudd:thread:%t %M %N:%l rehydrated "
                  "%u endpoint(s) from ds (%u restored, %u healed)\n"),
                  static_cast<unsigned>(restored + healed),
                  static_cast<unsigned>(restored),
                  static_cast<unsigned>(healed)));
    return restored + healed;
}

} // namespace

int main(int argc, char** argv) {
    // ── CLI args ──────────────────────────────────────────────────
    std::string ds_path = "/var/run/iot/data_store.sock";
    std::string vpn_subnet = "10.9.0.0/24";
    int proxy_port_start = 10000;   // above CoAP 5683/5684; small window —
    int proxy_port_end   = 10050;   // each published port = one docker-proxy
    int sync_interval    = 10;  // seconds

    for (int i = 1; i < argc; ++i) {
        std::string a{argv[i]};
        if (a.rfind("ds-socket=", 0) == 0)
            ds_path = a.substr(10);
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

    // Proxy-port range is ds-driven (cloud.vpn.proxy.port.{start,end}); the
    // CLI arg / built-in default is only the fallback so the range is never
    // hardcoded. Seed the effective values back so they're visible/editable
    // in ds (idempotent: an operator-set value is read then re-set unchanged).
    // NB: the docker-compose port-publish range must still cover this — Docker
    // can't read ds, so that publish stays a deployment knob.
    proxy_port_start = ds_int(ds, "cloud.vpn.proxy.port.start", proxy_port_start);
    proxy_port_end   = ds_int(ds, "cloud.vpn.proxy.port.end",   proxy_port_end);
    ds.set("cloud.vpn.proxy.port.start",
           data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});
    ds.set("cloud.vpn.proxy.port.end",
           data_store::Value{static_cast<std::uint32_t>(proxy_port_end)});

    // Tunnel subnet is ds-driven too (cloud.vpn.subnet, schema default
    // 10.9.0.0/24) — no env/CLI needed. ds is the single source of truth so a
    // VPN-page edit isn't clobbered on restart; seed the effective value back
    // so it's visible/editable.
    vpn_subnet = ds_str(ds, "cloud.vpn.subnet", vpn_subnet);
    ds.set("cloud.vpn.subnet", data_store::Value{vpn_subnet});

    // ── Core services ─────────────────────────────────────────────
    server::openvpn::VpnRegistry vpn_reg(vpn_subnet,
        static_cast<std::uint16_t>(proxy_port_start),
        static_cast<std::uint16_t>(proxy_port_end));
    server::lwm2m::EndpointRegistry ep_reg;
    server::lwm2m::BootstrapProvisioner provisioner(ep_reg, vpn_reg);

    // Restore persisted endpoints into the in-memory registries BEFORE the
    // main loop's first sync — otherwise the empty registry overwrites
    // cloud.endpoints with [] and every restart "loses" provisioning.
    rehydrate_registry(ds, ep_reg, vpn_reg, provisioner);

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
    // OTA: cloud-ui writes cloud.update.request; validate it into per-endpoint
    // jobs (cloud.update.pending) that the lwm2m-dm push tick consumes.
    auto ws_update = ds.watch("cloud.update.request", 1000);
    if (!ws_update.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l watch "
                            "cloud.update.request failed: %C\n"),
                   ws_update.err.c_str()));
    }

    // Seed initial VPN config in ds (subnet already seeded above from ds).
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
    // DNS resolver pushed to devices (so the device surfaces vpn.assigned.dns).
    // Empty disables the push; default 1.1.1.1 so the field is populated.
    ovpn_cfg.dns       = ds_str(ds, "cloud.vpn.dns", "1.1.1.1");
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
        {"cloud.vpn.dns",         data_store::Value{ovpn_cfg.dns}},
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
    // ds is the persistent source of truth for the runtime PKI; the iot-vpn
    // volume is disposable. Restore the CA + server cert from ds into the
    // working files BEFORE ensure() so a wiped/recreated iot-vpn volume yields
    // the SAME CA — otherwise it regenerates and invalidates every already-
    // pushed device cert. ds (iot-lib) survives redeploys.
    {
        const std::string caKey  = ds_str(ds, "cloud.vpn.ca.key.pem",     "");
        const std::string caCrt  = ds_str(ds, "cloud.vpn.ca.crt.pem",     "");
        const std::string srvKey = ds_str(ds, "cloud.vpn.server.key.pem", "");
        const std::string srvCrt = ds_str(ds, "cloud.vpn.server.crt.pem", "");
        if (!caKey.empty() && !caCrt.empty() && read_file(capaths.ca_key).empty()) {
            write_file(capaths.ca_key, caKey, 0600);
            write_file(capaths.ca_crt, caCrt, 0644);
            if (!srvKey.empty()) write_file(capaths.srv_key, srvKey, 0600);
            if (!srvCrt.empty()) write_file(capaths.srv_crt, srvCrt, 0644);
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l restored runtime PKI "
                                "from ds (iot-vpn volume not required)\n")));
        }
    }

    server::openvpn::CertAuthority cert_ca(capaths);
    if (!cert_ca.ensure()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l runtime CA ensure() "
                            "failed — per-device client certs won't be mintable\n")));
    }
    // Persist the runtime PKI back to ds so it survives an iot-vpn volume loss
    // (idempotent: re-sets the same PEMs when unchanged). Stored under
    // gid:cloud-svc, like the per-device keys in cloud.endpoint.credentials.
    if (cert_ca.have_ca()) {
        ds.set({
            {"cloud.vpn.ca.key.pem",     data_store::Value{read_file(capaths.ca_key)}},
            {"cloud.vpn.ca.crt.pem",     data_store::Value{read_file(capaths.ca_crt)}},
            {"cloud.vpn.server.key.pem", data_store::Value{read_file(capaths.srv_key)}},
            {"cloud.vpn.server.crt.pem", data_store::Value{read_file(capaths.srv_crt)}},
        });
    }

    server::openvpn::OpenVpnServer ovpn(ovpn_cfg);

    // Bring openvpn to the operator-desired state and publish it. Runs on
    // the main thread only (single owner of the child process / waitpid).
    // Crash-backoff state for openvpn supervision. The child runs in the
    // foreground; if it dies (bad TUN/cert/config) we restart it — but with
    // exponential backoff and the captured exit reason logged, so a
    // persistent failure no longer silently respawns every tick.
    int  ovpn_fails = 0;
    auto ovpn_last_start  = std::chrono::steady_clock::time_point{};
    auto ovpn_retry_after = std::chrono::steady_clock::time_point{};
    auto supervise_ovpn = [&]() {
        bool enabled = true;
        {
            std::vector<data_store::Client::GetResult> got;
            if (ds.get({"services.cloud.openvpn.server.enable"}, got).ok &&
                !got.empty() && got[0].has_value)
                if (auto b = data_store::to_bool(got[0].value)) enabled = *b;
        }
        std::string state;
        if (!enabled) {
            if (ovpn.running()) ovpn.stop();
            ovpn_fails = 0;
            ovpn_last_start  = {};
            ovpn_retry_after = {};
            state = "disabled";
        } else if (ovpn.running()) {
            // Clear the backoff only after a SUSTAINED run (>=60s). A process
            // that survives a few ticks but keeps dying inside the 30s "quick"
            // window is a crash loop, not stable — resetting every running tick
            // (the previous behaviour) pinned ovpn_fails at 1 so the backoff
            // never escalated ("failure #1 … retry in 2s" forever).
            if (ovpn_last_start.time_since_epoch().count() != 0 &&
                (std::chrono::steady_clock::now() - ovpn_last_start)
                    >= std::chrono::seconds(60)) {
                ovpn_fails = 0;
            }
            state = "running";
        } else {
            const auto now = std::chrono::steady_clock::now();
            // Transition running → exited: diagnose the crash exactly once,
            // schedule a backoff, and clear the tracker so subsequent ticks
            // in the backoff window stay quiet.
            if (ovpn_last_start.time_since_epoch().count() != 0) {
                const bool quick = (now - ovpn_last_start) < std::chrono::seconds(30);
                ovpn_fails = quick ? ovpn_fails + 1 : 1;
                const int backoff = std::min(60, 1 << std::min(ovpn_fails, 6)); // 2..60s
                ovpn_retry_after = now + std::chrono::seconds(backoff);
                const std::string tail = ovpn.log_tail();
                ACE_ERROR((LM_ERROR, ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                           "openvpn server exited rc=%d (failure #%d); next retry "
                           "in %ds. Captured output:\n%s\n"),
                           ovpn.exit_code(), ovpn_fails, backoff,
                           tail.empty() ? "(none captured)" : tail.c_str()));
                ovpn_last_start = {};       // handled
            }
            if (now >= ovpn_retry_after) {
                if (ovpn.start()) {
                    ovpn_last_start = now;
                    ACE_DEBUG((LM_INFO, ACE_TEXT("%D cloudd:thread:%t %M %N:%l "
                               "openvpn server (re)started\n")));
                }
            }
            state = ovpn.running() ? "running" : "exited";
        }
        g_ovpn_pid.store(ovpn.running() ? ovpn.pid() : 0);
        ds.set("services.cloud.openvpn.server.state",
               data_store::Value{state});
    };
    supervise_ovpn();   // initial start

    // ── Device-UI-over-VPN DNAT (per-device nftables) ─────────────
    // iot-cloudd runs the openvpn server, so tun0 + CAP_NET_ADMIN live in
    // this netns — the right place to install cloud:<proxy_port> →
    // <tun_ip>:<ui_port> DNAT. Enable IPv4 forwarding once so the DNAT'd
    // connection routes out over the tunnel, then rebuild the full ruleset
    // from the (just-restored) endpoint registry so a restart re-installs
    // every device's mapping atomically. openvpn names the first tun device
    // <dev>0 (e.g. "tun" → "tun0").
    const std::string tun_dev = ovpn_cfg.dev + "0";
    // Seed the ui_port key so it's visible in the cloud UI / ds-cli.
    ds.set("cloud.proxy.device.ui.port",
           data_store::Value{static_cast<std::int32_t>(
               ds_int(ds, "cloud.proxy.device.ui.port", 80))});
    if (!server::dnat::enable_ip_forward()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l could not enable "
                            "net.ipv4.ip_forward (device-UI DNAT may not route)\n")));
    }
    // Reconstruct the full rule set from the persisted cloud.endpoints so a
    // restart restores every device's mapping atomically before any sync.
    rebuild_device_dnat(ds, tun_dev);

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
                // Ignore an empty request: clearing the trigger (below) sets it
                // to "" and re-fires this watch; processing "" would provision a
                // bogus empty-serial endpoint.
                if (auto ep = data_store::to_string(ev.value); ep && !ep->empty()) {
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
                                        cur, *ep, mc->ca_crt,
                                        mc->client_crt, mc->client_key);
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
                    // One-shot: clear the request so a watch replay on cloudd
                    // restart doesn't re-provision (and resurrect) a since-
                    // deprovisioned endpoint. The "" re-fire is ignored above.
                    ds.set("cloud.provision.request",
                           data_store::Value{std::string("")});
                }
            } else if (ev.key == "cloud.deprovision.request") {
                if (auto ep = data_store::to_string(ev.value); ep && !ep->empty()) {
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D cloudd:thread:%t %M %N:%l deprovision request '%C'\n"),
                               ep->c_str()));
                    bool ok = provisioner.deprovision(*ep);
                    // Also drop the per-endpoint credential record so the BS/DM
                    // stop accepting it and the row disappears from
                    // cloud.endpoint.credentials. Done independently of the
                    // registry result so a stale credential-only entry can
                    // still be cleaned (counts as a successful removal).
                    try {
                        const std::string cur =
                            ds_str(ds, "cloud.endpoint.credentials", "[]");
                        const std::string next =
                            server::lwm2m::remove_credential(cur, *ep);
                        if (next != cur) {
                            ds.set("cloud.endpoint.credentials",
                                   data_store::Value{next});
                            ok = true;
                        }
                    } catch (const std::exception& e) {
                        ACE_ERROR((LM_ERROR,
                                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l remove "
                                            "credential for %C failed: %C\n"),
                                   ep->c_str(), e.what()));
                    }
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D cloudd:thread:%t %M %N:%l deprovision %C '%C'\n"),
                               (ok ? "ok" : "failed (not found)"), ep->c_str()));
                    // One-shot: clear both triggers so a watch replay on restart
                    // doesn't re-deprovision, and a stale provision.request can't
                    // resurrect what we just removed.
                    ds.set("cloud.deprovision.request",
                           data_store::Value{std::string("")});
                    ds.set("cloud.provision.request",
                           data_store::Value{std::string("")});
                }
            } else if (ev.key == "log.level") {
                g_log.apply_level(ds);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D cloudd:thread:%t %M %N:%l log level changed\n")));
            } else if (ev.key == "cloud.lwm2m.registrations") {
                // lwm2m-dm published a registration change — fold online/
                // offline into the endpoint registry before the sync below.
                reconcile_registrations(ds, ep_reg);
            } else if (ev.key == "cloud.update.request") {
                // OTA: validate the request against the firmware manifest +
                // known endpoints, then emit per-endpoint jobs for lwm2m-dm.
                std::string reqJson;
                if (auto s = data_store::to_string(ev.value)) reqJson = *s;
                if (!reqJson.empty()) {
                    try {
                        auto req     = nlohmann::json::parse(reqJson);
                        auto serials = req.value("serials", nlohmann::json::array());
                        std::string url = req.value("url", "");
                        std::string sha = req.value("sha256", "");
                        std::string ver = req.value("version", "");
                        // Validate url against the manifest (and fill sha/ver).
                        bool known = false;
                        auto man = nlohmann::json::parse(
                            ds_str(ds, "cloud.firmware.manifest", "[]"));
                        if (man.is_array()) for (const auto& m : man) {
                            if (m.value("ipk_url", "") == url) {
                                known = true;
                                if (sha.empty()) sha = m.value("sha256",  "");
                                if (ver.empty()) ver = m.value("version", "");
                                break;
                            }
                        }
                        if (known && serials.is_array() && !url.empty()) {
                            auto epj = nlohmann::json::parse(
                                ds_str(ds, "cloud.endpoints", "[]"));
                            auto ep_known = [&epj](const std::string& s) {
                                if (epj.is_array()) for (const auto& e : epj)
                                    if (e.value("endpoint", "") == s) return true;
                                return false;
                            };
                            std::string fullUrl = url +
                                (url.find('?') == std::string::npos ? "?" : "&") +
                                "sha256=" + sha + "&version=" + ver;
                            nlohmann::json pending = nlohmann::json::array();
                            nlohmann::json status  = nlohmann::json::array();
                            for (const auto& s : serials) {
                                if (!s.is_string()) continue;
                                std::string serial = s.get<std::string>();
                                if (!ep_known(serial)) continue;
                                pending.push_back({{"endpoint", serial},
                                    {"url", fullUrl}, {"sha256", sha}, {"version", ver}});
                                status.push_back({{"serial", serial}, {"state", 1},
                                    {"result", 0}, {"version", ver}, {"ts", 0}});
                            }
                            ds.set("cloud.update.pending",
                                   data_store::Value{pending.dump()});
                            ds.set("cloud.update.status",
                                   data_store::Value{status.dump()});
                            ACE_DEBUG((LM_INFO,
                                ACE_TEXT("%D cloudd:thread:%t %M %N:%l queued %u OTA "
                                         "job(s) ver=%C\n"),
                                static_cast<unsigned>(pending.size()), ver.c_str()));
                        } else {
                            ACE_ERROR((LM_ERROR,
                                ACE_TEXT("%D cloudd:thread:%t %M %N:%l OTA request "
                                         "rejected (unknown url or bad shape)\n")));
                        }
                    } catch (const std::exception& e) {
                        ACE_ERROR((LM_ERROR,
                            ACE_TEXT("%D cloudd:thread:%t %M %N:%l OTA request parse "
                                     "failed: %C\n"), e.what()));
                    }
                    ds.set("cloud.update.request", data_store::Value{std::string()});
                }
            }
            // Sync after every event so the UI sees changes quickly, then
            // rebuild the DNAT ruleset from the fresh endpoint set (a
            // provision/deprovision/online-change may have added/removed a
            // device → its cloud:<proxy_port> mapping must follow).
            sync_endpoints_to_ds(ds, ep_reg);
            rebuild_device_dnat(ds, tun_dev);
            sync_tick = 0;
        } else {
            // recv_event timed out — periodic housekeeping
            supervise_ovpn();   // restart on crash / honor enable flips

            // Publish which devices have a live VPN tunnel (from the openvpn
            // management interface). lwm2m-dm reads this and stops pushing the
            // cert once the device's tunnel is up — the end-goal "done" signal,
            // and it needs no server→device request.
            {
                nlohmann::json arr = nlohmann::json::array();
                for (auto& s : poll_vpn_connected(ovpn_cfg.mgmt_port)) arr.push_back(s);
                ds.set("cloud.vpn.connected", data_store::Value{arr.dump()});
            }

            if (++sync_tick >= 3) {
                sync_tick = 0;
                // Safety-net reconcile in case a watch notification was missed.
                reconcile_registrations(ds, ep_reg);
                sync_endpoints_to_ds(ds, ep_reg);
                // Safety-net DNAT rebuild — also picks up a live
                // cloud.proxy.device.ui.port change.
                rebuild_device_dnat(ds, tun_dev);
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
