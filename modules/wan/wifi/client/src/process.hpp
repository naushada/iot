#ifndef __wifi_client_process_hpp__
#define __wifi_client_process_hpp__

/// Subprocess lifecycle for wifi-client (L15/D5).
///
/// Two children are spawned during a normal session:
///
///   1. wpa_supplicant -i <iface> -c <generated.conf>
///                     -C <ctrl-dir> -D nl80211,wext
///   2. udhcpc -i <iface> -f -q   (or dhclient -d <iface>)
///
/// Both are wrapped by `Process`, the wifi-client analog of
/// openvpn_client::OpenVpnProcess. One class instance owns one
/// subprocess. The Supervisor (D6) holds two instances so the
/// lifecycle of the supplicant child and the DHCP child are
/// independent.
///
/// The generated wpa_supplicant.conf is built from the parsed
/// `wifi.networks` JSON via `build_wpa_supplicant_config()`.
/// Tests drive the writer with `/bin/sh -c …` instead of a real
/// wpa_supplicant binary, matching openvpn-client's approach.
///
/// REQ-WIFI-014..017, NFR-WIFI-006 close here.

#include <chrono>
#include <string>
#include <sys/types.h>          // pid_t
#include <vector>

namespace wifi_client {

/// One entry from `wifi.networks` JSON. Parsed by
/// `parse_wifi_networks`; passed by value into the config writer.
struct WifiNetwork {
    std::string  ssid;
    std::string  psk;
    int          priority = 0;
    /// "WPA-PSK" (default), "NONE" (open network), or "WPA-EAP"
    /// (WPA-Enterprise). Anything else is propagated verbatim to
    /// wpa_supplicant.conf so future key management modes don't need
    /// a code change here.
    std::string  key_mgmt = "WPA-PSK";

    // ───── WPA-Enterprise (key_mgmt == "WPA-EAP") fields ─────
    // Populated by parse_wifi_networks only for EAP entries; ignored
    // for WPA-PSK / NONE. The config writer emits them in place of psk.
    std::string  eap;        ///< "PEAP" (default) / "TTLS" / "TLS" / …
    std::string  identity;   ///< EAP identity (username)
    std::string  password;   ///< EAP password
    std::string  phase2;     ///< inner auth, e.g. "auth=MSCHAPV2"
    std::string  ca_cert;    ///< optional CA-cert path (emitted iff set)
};

/// Parse a wifi.networks JSON string into a vector of WifiNetwork.
/// Empty / "[]" input returns an empty vector. Returns the parsed
/// list on success; on bad shape sets `err_out` (when provided) to
/// a descriptive message (suitable for wifi.last.error) and returns
/// an empty vector. Validates:
///   - root must be a JSON array
///   - each entry must have "ssid" (non-empty string)
///   - each entry must have "psk" unless "key_mgmt" is "NONE" or
///     "WPA-EAP"
///   - WPA-EAP entries must have "identity" and "password"; "eap"
///     defaults to "PEAP" and "phase2" to "auth=MSCHAPV2"; "ca_cert"
///     is optional
///   - "priority" is optional, defaults to 0
///   - "key_mgmt" is optional, defaults to "WPA-PSK"
std::vector<WifiNetwork> parse_wifi_networks(const std::string& json,
                                             std::string*       err_out = nullptr);

/// Render a wpa_supplicant.conf from `networks`. The list is
/// sorted by descending priority before emit so the supplicant
/// prefers the highest-priority SSID. Empty list returns the
/// header (ctrl_interface=..., update_config=0) with no
/// network={} blocks — wpa_supplicant accepts this and just
/// sits idle, which lets the daemon stay up while the operator
/// configures.
std::string build_wpa_supplicant_config(const std::string&             iface,
                                        const std::string&             ctrl_dir,
                                        const std::vector<WifiNetwork>& networks);

/// Build the DHCP-client argv for `dhcp_path` against `iface`. Shape is
/// chosen by the binary basename:
///   udhcpc:   <path> -i <iface> -f -q [-s <script>]
///   dhclient: <path> -d <iface>
/// `script` (udhcpc only, when non-empty) is the `-s` lease hook. Factored
/// out of spawn_dhcp so the argv shaping is unit-testable without a spawn.
std::vector<std::string> build_dhcp_argv(const std::string& dhcp_path,
                                         const std::string& iface,
                                         const std::string& script = "");

/// Write `body` to a fresh /tmp/wpa-XXXXXX.conf and return the path.
/// Throws std::runtime_error on mkstemps / write failure. Same
/// $TMPDIR → /tmp → cwd fallback chain openvpn-client uses.
std::string write_temp_config(const std::string& body);

/// Pick the DHCP-client binary based on `scheme`:
///   "udhcpc"   → /usr/bin/udhcpc           (or override)
///   "dhclient" → /usr/sbin/dhclient        (or override)
///   "auto"     → probe udhcpc first, dhclient second, fall back
///                to whichever exists. Empty string returned when
///                neither is on disk.
/// `override_path` (when non-empty) wins unconditionally — operators
/// who install a third-party DHCP client point at it directly.
std::string pick_dhcp_client(const std::string& scheme,
                             const std::string& override_path);

/// Thin RAII wrapper around an `ACE_Process`. Owns one child's
/// pid, remembers any generated config path, ensures terminate()
/// + reap on destruction so a daemon-side throw can't leave a
/// zombie wpa_supplicant or udhcpc behind.
class Process {
public:
    Process();
    ~Process();

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    /// Generic spawn — exec `executable` with `argv`. argv[0] is
    /// the program name the child will see (convention: same as
    /// `executable`). Used by tests with /bin/sh stand-ins.
    bool spawn(const std::string&              executable,
               const std::vector<std::string>& argv);

    /// Build wpa_supplicant.conf from `networks`, write to a temp
    /// file, then spawn `wpa_path` with `-i <iface> -c <tmp>
    /// -C <ctrl_dir>`. The temp path is unlinked in the destructor.
    /// driver_list passed verbatim to `-D` (default "nl80211,wext").
    bool spawn_wpa_supplicant(const std::string&             wpa_path,
                              const std::string&             iface,
                              const std::string&             ctrl_dir,
                              const std::vector<WifiNetwork>& networks,
                              const std::string&             driver_list = "nl80211,wext");

    /// Spawn a DHCP client child against `iface`. `dhcp_path` is
    /// the absolute path to udhcpc or dhclient as picked by
    /// pick_dhcp_client(). The argv is shaped per the client:
    ///   udhcpc:   -i <iface> -f -q [-s <script>]
    ///   dhclient: -d <iface>
    /// The picker baseline is the binary's *basename* — anything
    /// containing "udhcpc" gets the udhcpc shape; anything else
    /// falls to dhclient shape.
    /// `script` (when non-empty AND udhcpc) is passed as `-s <script>`,
    /// the lease hook that mirrors the lease into the data-store
    /// (udhcpc-ds.script). Empty = udhcpc's built-in default script.
    /// dhclient ignores `script` (different hook mechanism).
    bool spawn_dhcp(const std::string& dhcp_path,
                    const std::string& iface,
                    const std::string& script = "");

    /// Subprocess pid (0 if no spawn yet).
    pid_t pid() const { return m_pid; }

    /// Cheap liveness probe — non-blocking waitpid. Returns true
    /// if the child is still running. Caches the exit code on
    /// observed exit.
    bool running();

    /// Block until the child exits. Returns the exit code (0..255)
    /// or -1 if killed by a signal. Idempotent.
    int wait();

    /// SIGTERM the child; if still alive after `grace`, SIGKILL.
    /// Reaps so no zombie. No-op if there's no child. The default
    /// grace of 5s satisfies NFR-WIFI-006.
    void terminate(std::chrono::milliseconds grace =
                       std::chrono::seconds(5));

    /// Path of the generated wpa_supplicant.conf when
    /// spawn_wpa_supplicant() was used; empty otherwise.
    const std::string& config_path() const { return m_config_path; }

private:
    pid_t       m_pid = 0;
    int         m_exit_code = -1;
    bool        m_waited = false;
    std::string m_config_path;
};

} // namespace wifi_client

#endif /* __wifi_client_process_hpp__ */
