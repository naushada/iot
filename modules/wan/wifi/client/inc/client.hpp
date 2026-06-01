#ifndef __wifi_client_client_hpp__
#define __wifi_client_client_hpp__

/// Public-ish header for the wifi-client module. Only really "public"
/// in the sense that the in-tree test target links against the same
/// lib (wifi_client_lib) the binary uses — there is no API stability
/// contract for external consumers.
///
/// L15/D1 scaffold: CLI parse + `--dump` keylist diagnostic.
/// D3..D6 add DsBridge, ctrl protocol parser, process wrapper, and
/// the lifecycle FSM as their phases land.

#include <iosfwd>
#include <string>

namespace wifi_client {

/// Status surfaced by the v0 main. Mirrors openvpn_client::Status +
/// data_store::Status shape so callers can switch on `.ok` + read `.err`.
struct Status {
    bool        ok = true;
    int         code = 0;
    std::string err;
};

/// Result of parsing argv. Pure data — `main` dispatches; `parse_cli`
/// stays unit-testable without spawning the binary.
struct ParsedCli {
    std::string sock;
    std::string wpa_path  = "/usr/sbin/wpa_supplicant";
    std::string iface     = "wlan0";
    std::string ctrl_dir  = "/run/wpa_supplicant";
    bool        dump      = false;
    bool        once      = false;
    bool        help      = false;
    /// Non-zero when argv was malformed. `err` carries the diagnostic
    /// main.cpp prints to stderr before exiting with this code.
    /// 0 = ok; 2 = unknown / malformed argument.
    int         exit_code = 0;
    std::string err;
};

/// Parse argv into a ParsedCli. Recognises:
///   --ds-sock=PATH  --wpa=PATH  --iface=NAME  --ctrl-dir=DIR
///   --dump  --once  --help / -h
/// Unknown flags MUST land as `exit_code=2` with a descriptive `err`.
ParsedCli parse_cli(int argc, char** argv);

/// Help text. Streamed to `out` so tests don't need to capture cout.
void print_usage(std::ostream& out);

/// Diagnostic mode: list every wifi.* key this daemon reads and
/// writes, exit 0 without contacting ds-server. REQ-WIFI-003.
///
/// The `socketPath` argument is accepted-but-ignored so the dispatch
/// in main.cpp can pass it uniformly; once ds-server contact is on
/// the table for a v1 of this op, the signature won't change.
Status v0_dump_wifi_keys(const std::string& socketPath,
                         std::ostream&      out);

/// Full lifecycle: connect to ds-server, refuse on missing_required()
/// (today: nothing required — wifi.iface defaults to "wlan0"), spawn
/// wpa_supplicant(8), connect to its control socket, route CTRL-EVENT
/// lines through Lifecycle::step into DsBridge writes. Spawns a DHCP
/// client child once CTRL-EVENT-CONNECTED arrives. Quiesces on
/// subprocess exit (or after the first CTRL-EVENT-CONNECTED if `once`).
///
/// D1 stub: returns `ok=false code=75` until D3..D6 land.
Status run_daemon(const std::string& socketPath,
                  const std::string& wpa_path = "/usr/sbin/wpa_supplicant",
                  const std::string& iface    = "wlan0",
                  const std::string& ctrl_dir = "/run/wpa_supplicant",
                  bool               once     = false);

} // namespace wifi_client

#endif /* __wifi_client_client_hpp__ */
