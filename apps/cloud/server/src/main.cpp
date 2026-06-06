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
        std::string_view a{argv[i]};
        if (a.starts_with("ds-socket="))
            ds_path = a.substr(10);
        else if (a.starts_with("vpn-subnet="))
            vpn_subnet = a.substr(11);
        else if (a.starts_with("proxy-start="))
            proxy_port_start = std::stoi(std::string(a.substr(13)));
        else if (a.starts_with("proxy-end="))
            proxy_port_end = std::stoi(std::string(a.substr(11)));
        else if (a.starts_with("sync-interval="))
            sync_interval = std::stoi(std::string(a.substr(14)));
    }

    // ── Connect to ds-server ──────────────────────────────────────
    data_store::Client ds;
    auto cs = ds.connect(ds_path);
    if (!cs.ok) {
        std::cerr << "cloudd: ds-server connect failed: " << cs.err << "\n";
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

    std::cout << "cloudd: started, ds=" << ds_path
              << " vpn-subnet=" << vpn_subnet
              << " proxy=" << proxy_port_start << "-" << proxy_port_end << "\n";

    // ── Main loop ─────────────────────────────────────────────────
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(sync_interval));

        // Sync endpoint registry → data store
        sync_endpoints_to_ds(ds, ep_reg);

        // Write VPN config to ds
        ds.set("cloud.vpn.subnet", data_store::Value{vpn_subnet});
        ds.set("cloud.vpn.port.next",
               data_store::Value{static_cast<std::uint32_t>(proxy_port_start)});
    }

    std::cout << "cloudd: stopped\n";
    return 0;
}
