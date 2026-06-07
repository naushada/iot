/// wifi-client v0 main implementation (L15/D1 scaffold).
///
/// D1 ships:
///   - parse_cli: argv → ParsedCli (testable without spawning).
///   - v0_dump_wifi_keys: REQ-WIFI-003 — list every wifi.* key the
///     daemon will read/write, exit 0 without contacting ds-server.
///   - run_daemon: D1 stub returning a typed "not yet implemented"
///     Status so operators can't be confused into thinking nothing
///     is happening. D3..D6 land the real lifecycle.

#include "client.hpp"

#include <array>
#include <cstddef>
#include <ostream>
#include <string_view>

#include <ace/Log_Msg.h>

#include "ds_bridge.hpp"
#include "supervisor.hpp"

#include "data_store/client.hpp"
#include "data_store/stats_publisher.hpp"

namespace wifi_client {

namespace {

/// Every wifi.* key the daemon will touch, broken out by direction
/// so `--dump` can print one section per role. The lists are the
/// authoritative source for the D2 schema (schemas/wifi.lua); when
/// D2 lands it MUST agree with these entries.
constexpr std::array<std::string_view, 9> kReadKeys = {
    "wifi.iface",
    "wifi.ctrl.dir",
    "wifi.wpa.path",
    "wifi.networks",
    "wifi.scan.interval.sec",
    "wifi.scan.max.results",
    "wifi.scan.request",
    "wifi.dhcp.client",
    "wifi.dhcp.path",
};

constexpr std::array<std::string_view, 11> kWriteKeys = {
    "wifi.assoc.state",
    "wifi.assoc.ssid",
    "wifi.assoc.bssid",
    "wifi.signal.rssi",
    "wifi.scan.results",
    "wifi.scan.last.unix",
    "wifi.dhcp.state",
    "wifi.dhcp.ip",
    "wifi.pid.wpa",
    "wifi.pid.dhcp",
    "wifi.last.error",
};

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size()
        && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

void print_usage(std::ostream& out) {
    out <<
        "wifi-client (L15/D1)\n"
        "\n"
        "Usage: wifi-client [--ds-sock=PATH] [--wpa=PATH] [--iface=NAME]\n"
        "                   [--ctrl-dir=DIR] [--dump] [--once] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket; defaults to ds-server's\n"
        "                   built-in default (/var/run/iot/data_store.sock).\n"
        "  --wpa=PATH       path to wpa_supplicant(8); defaults to\n"
        "                   /usr/sbin/wpa_supplicant.\n"
        "  --iface=NAME     wifi iface to manage; defaults to wlan0.\n"
        "  --ctrl-dir=DIR   wpa_supplicant control-socket dir; defaults to\n"
        "                   /run/wpa_supplicant.\n"
        "  --dump           snapshot wifi.* keys and exit (diagnostic).\n"
        "  --once           exit after the first CTRL-EVENT-CONNECTED (smoke).\n"
        "  --help           show this and exit.\n";
}

ParsedCli parse_cli(int argc, char** argv) {
    ParsedCli pc;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (starts_with(a, "--ds-sock=")) {
            pc.sock = std::string(a.substr(10));
        } else if (starts_with(a, "--wpa=")) {
            pc.wpa_path = std::string(a.substr(6));
        } else if (starts_with(a, "--iface=")) {
            pc.iface = std::string(a.substr(8));
        } else if (starts_with(a, "--ctrl-dir=")) {
            pc.ctrl_dir = std::string(a.substr(11));
        } else if (a == "--dump") {
            pc.dump = true;
        } else if (a == "--once") {
            pc.once = true;
        } else if (a == "--help" || a == "-h") {
            pc.help = true;
        } else {
            pc.exit_code = 2;
            pc.err = "wifi-client: unknown argument '" + std::string(a) + "'";
            return pc;
        }
    }
    return pc;
}

Status v0_dump_wifi_keys(const std::string& /*socketPath*/,
                         std::ostream&      out) {
    // REQ-WIFI-003: --dump MUST exit 0 without contacting ds-server.
    // Operator-facing output streams to `out` (cout from main, a
    // stringstream from gtest). That's the documented carve-out from
    // feedback_ace_logging.md — CLI output stays on stdout, only
    // runtime diagnostics route through ACE_DEBUG/ACE_ERROR.
    out << "wifi-client wifi.* keys (L15)\n";
    out << "\n";
    out << "Read keys (operator -> daemon):\n";
    for (const auto& k : kReadKeys) {
        out << "  " << k << "\n";
    }
    out << "\n";
    out << "Write keys (daemon -> operator):\n";
    for (const auto& k : kWriteKeys) {
        out << "  " << k << "\n";
    }
    return {};
}

Status run_daemon(const std::string& socketPath,
                  const std::string& wpa_path,
                  const std::string& iface,
                  const std::string& ctrl_dir,
                  bool               once) {
    DsBridge ds(socketPath);
    if (!ds.connected()) {
        Status s; s.ok = false; s.code = 1;
        s.err = "data-store connect failed"; return s;
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [wifi:%t] %M %N:%l starting on iface=%C "
                        "wpa=%C ctrl_dir=%C\n"),
               iface.c_str(), wpa_path.c_str(), ctrl_dir.c_str()));

    // L22 — resource telemetry. Blocking supervisor loop (no ACE reactor),
    // so StatsPublisher spawns its own ACE_Task thread for the singleton
    // reactor timer (run_reactor_thread=true, the default). wpa_supplicant +
    // udhcpc run in this container, folded into these cgroup totals.
    data_store::StatsPublisher stats(
        "services.wifi.client",
        [&ds](const std::vector<data_store::KV>& kv) {
            if (auto* c = ds.client()) c->set(kv);
        });
    if (ds.client()) stats.open();

    SupervisorOptions opt;
    opt.wpa_path  = wpa_path;
    opt.iface     = iface;
    opt.ctrl_dir  = ctrl_dir;
    opt.once      = once;
    Supervisor sup(ds, std::move(opt));
    int rc = sup.run();
    Status st;
    st.ok   = (rc == 0);
    st.code = rc;
    return st;
}

// Test-only accessors so main_test.cpp can assert the keylist
// without re-declaring it (which would defeat the test). Returning
// pointer+size keeps the header free of <array>.
const std::string_view* read_keys_data()    { return kReadKeys.data(); }
std::size_t             read_keys_size()    { return kReadKeys.size(); }
const std::string_view* write_keys_data()   { return kWriteKeys.data(); }
std::size_t             write_keys_size()   { return kWriteKeys.size(); }

} // namespace wifi_client
