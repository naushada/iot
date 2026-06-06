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
#include "bootstrap.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <ace/Log_Msg.h>

#include <atomic>
#include <csignal>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

// Sync endpoint registry to cloud.endpoints JSON in the data store.
void sync_endpoints_to_ds(data_store::Client& ds,
                           server::lwm2m::EndpointRegistry& reg) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ep : reg.list_all()) {
        nlohmann::json item;
        item["endpoint"]    = ep.ep;
        item["state"]       = ep.registered ? "online" : "offline";
        item["tun_ip"]      = ep.tun_ip;
        item["proxy_port"]  = ep.proxy_port;
        item["registered"]  = ep.registered;
        arr.push_back(item);
    }
    ds.set("cloud.endpoints", data_store::Value{arr.dump()});
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
            proxy_port_start = std::stoi(a.substr(13));
        else if (a.rfind("proxy-end=", 0) == 0)
            proxy_port_end = std::stoi(a.substr(11));
        else if (a.rfind("sync-interval=", 0) == 0)
            sync_interval = std::stoi(a.substr(14));
    }

    // ── Connect to ds-server ──────────────────────────────────────
    data_store::Client ds;
    auto cs = ds.connect(ds_path);
    if (!cs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [cloudd:%t] %M %N:%l ds-server connect failed: %C\n"),
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
                   ACE_TEXT("%D [cloudd:%t] %M %N:%l watch provision.request failed: %C\n"),
                   ws_prov.err.c_str()));
    }
    auto ws_depr = ds.watch("cloud.deprovision.request", 1000);
    if (!ws_depr.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [cloudd:%t] %M %N:%l watch deprovision.request failed: %C\n"),
                   ws_depr.err.c_str()));
    }

    // Seed initial VPN config in ds
    ds.set("cloud.vpn.subnet", data_store::Value{vpn_subnet});
    ds.set("cloud.vpn.port.next",
           data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});

    // Self-report running state to ds for Services page.
    // openvpn-server is managed internally by iot-cloudd — report its state too.
    ds.set("services.cloud.iot.cloudd.state", data_store::Value{std::string("running")});
    ds.set("services.cloud.openvpn.server.state", data_store::Value{std::string("running")});

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [cloudd:%t] %M %N:%l started, ds=%C vpn-subnet=%C"
                        " proxy=%d-%d sync-interval=%d\n"),
               ds_path.c_str(), vpn_subnet.c_str(),
               proxy_port_start, proxy_port_end, sync_interval));

    // ── Log level from data store (reused from device pattern) ─────
    // Priority mask maps the log.level string to ACE log priority.
    // Default is LM_INFO; operator changes via cloud UI take effect
    // on the next timeout tick.
    {
        std::vector<data_store::Client::GetResult> lg;
        auto ls = ds.get({"log.level"}, lg);
        if (ls.ok && !lg.empty() && lg[0].has_value) {
            if (auto lv = data_store::to_string(lg[0].value)) {
                unsigned long mask = LM_INFO;
                std::string upper = *lv;
                for (auto& c : upper) c = static_cast<char>(std::toupper(c));
                if (upper == "DEBUG")       mask = LM_DEBUG;
                else if (upper == "INFO")   mask = LM_INFO;
                else if (upper == "WARNING") mask = LM_WARNING;
                else if (upper == "ERROR")  mask = LM_ERROR | LM_CRITICAL;
                ACE_Log_Msg::instance()->priority_mask(
                    static_cast<int>(mask), ACE_Log_Msg::PROCESS);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D [cloudd:%t] %M %N:%l log level set to %C\n"),
                           upper.c_str()));
            }
        }
    }

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
                               ACE_TEXT("%D [cloudd:%t] %M %N:%l provision request '%C'\n"),
                               ep->c_str()));
                    auto result = provisioner.provision(*ep);
                    if (result.has_value()) {
                        ACE_DEBUG((LM_INFO,
                                   ACE_TEXT("%D [cloudd:%t] %M %N:%l provisioned %C"
                                            " tun=%C proxy=%d\n"),
                                   result->endpoint.c_str(), result->tun_ip.c_str(),
                                   static_cast<int>(result->proxy_port)));
                    } else {
                        ACE_ERROR((LM_ERROR,
                                   ACE_TEXT("%D [cloudd:%t] %M %N:%l provision failed"
                                            " '%C' (dup or exhausted)\n"),
                                   ep->c_str()));
                    }
                }
            } else if (ev.key == "cloud.deprovision.request") {
                if (auto ep = data_store::to_string(ev.value)) {
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D [cloudd:%t] %M %N:%l deprovision request '%C'\n"),
                               ep->c_str()));
                    bool ok = provisioner.deprovision(*ep);
                    ACE_DEBUG((LM_INFO,
                               ACE_TEXT("%D [cloudd:%t] %M %N:%l deprovision %C '%C'\n"),
                               (ok ? "ok" : "failed (not found)"), ep->c_str()));
                }
            }
            // Sync after every event so the UI sees changes quickly
            sync_endpoints_to_ds(ds, ep_reg);
            sync_tick = 0;
        } else {
            // recv_event timed out — periodic housekeeping
            if (++sync_tick >= 3) {
                sync_tick = 0;
                sync_endpoints_to_ds(ds, ep_reg);
                ds.set("cloud.vpn.port.next",
                       data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});
                // Reload log level so operator changes via cloud UI
                // take effect without a daemon restart.
                {
                    std::vector<data_store::Client::GetResult> lg;
                    auto ls = ds.get({"log.level"}, lg);
                    if (ls.ok && !lg.empty() && lg[0].has_value) {
                        if (auto lv = data_store::to_string(lg[0].value)) {
                            unsigned long mask = LM_INFO;
                            std::string upper = *lv;
                            for (auto& c : upper)
                                c = static_cast<char>(std::toupper(c));
                            if (upper == "DEBUG")       mask = LM_DEBUG;
                            else if (upper == "INFO")   mask = LM_INFO;
                            else if (upper == "WARNING") mask = LM_WARNING;
                            else if (upper == "ERROR")  mask = LM_ERROR | LM_CRITICAL;
                            ACE_Log_Msg::instance()->priority_mask(
                                static_cast<int>(mask), ACE_Log_Msg::PROCESS);
                        }
                    }
                }
            }
        }
    }

    ds.set("services.cloud.iot.cloudd.state", data_store::Value{std::string("exited")});
    ds.set("services.cloud.openvpn.server.state", data_store::Value{std::string("exited")});
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [cloudd:%t] %M %N:%l stopped\n")));
    return 0;
}
