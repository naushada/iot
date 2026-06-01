/// wifi-client — daemon entry (L15/D1).
///
/// Usage:
///   wifi-client [--ds-sock=PATH] [--wpa=PATH] [--iface=NAME]
///               [--ctrl-dir=DIR] [--dump] [--once] [--help]
///
/// Default (no --dump): full lifecycle — spawn wpa_supplicant(8),
/// connect to its control socket, route CTRL-EVENT-* lines into
/// wifi.* writes; spawn DHCP client on CTRL-EVENT-CONNECTED.
/// --once exits after the first CTRL-EVENT-CONNECTED (smoke tests).
/// --dump lists every wifi.* key the daemon touches and exits 0
/// without contacting ds-server.

#include <cstdlib>
#include <iostream>

#include "client.hpp"

int main(int argc, char** argv) {
    auto pc = wifi_client::parse_cli(argc, argv);
    if (pc.exit_code != 0) {
        std::cerr << pc.err << "\n";
        wifi_client::print_usage(std::cerr);
        return pc.exit_code;
    }
    if (pc.help) {
        wifi_client::print_usage(std::cout);
        return 0;
    }

    auto rs = pc.dump
            ? wifi_client::v0_dump_wifi_keys(pc.sock, std::cout)
            : wifi_client::run_daemon(pc.sock, pc.wpa_path, pc.iface,
                                      pc.ctrl_dir, pc.once);
    if (!rs.ok && !rs.err.empty()) {
        std::cerr << "wifi-client: " << rs.err << "\n";
    }
    return rs.ok ? 0 : 1;
}
