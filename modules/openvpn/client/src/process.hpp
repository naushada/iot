#ifndef __openvpn_client_process_hpp__
#define __openvpn_client_process_hpp__

/// Process lifecycle around openvpn(8) (or any subprocess — tests
/// use /bin/sh as a stand-in to avoid the tun-device + CAP_NET_ADMIN
/// requirement on a real openvpn invocation).
///
/// Built on ACE_Process + ACE_Process_Options. Pid + exit-code
/// observation is synchronous (wait() blocks); the D6 lifecycle wires
/// this onto the reactor via SIGCHLD or a periodic try_wait().
///
/// Config file lives at /tmp/openvpn-XXXXXX.conf (mkstemp). The plan
/// (L12 Q2) preferred /run/iot/, but that directory only exists when
/// the daemon runs under systemd's RuntimeDirectory= — fall back
/// gracefully for dev runs.

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>      // pid_t
#include <vector>

namespace openvpn_client {

/// Inputs to `build_openvpn_config()`. Mirrors the vpn.* schema keys
/// the DsBridge surfaces, but as a plain POD so this module doesn't
/// depend on DsBridge / data_store — keeps the unit tests pure.
struct OpenVpnConfig {
    std::string   remote_host;
    std::uint32_t remote_port  = 1194;
    std::string   remote_proto = "udp";
    std::string   cert_path;
    std::string   key_path;
    std::string   ca_path;
    std::string   cipher       = "AES-256-GCM";
    std::string   dev          = "tun";
    std::uint32_t mgmt_port    = 7505;
};

/// Render an OpenVPN client config from the inputs. Pure — caller
/// hands the result to write_temp_config() or any other writer. Adds
/// the `client`, `nobind`, `verb 3` baseline plus a `management
/// 127.0.0.1 <mgmt_port>` line so the daemon can attach.
std::string build_openvpn_config(const OpenVpnConfig& c);

/// Write `body` to a fresh /tmp/openvpn-XXXXXX.conf and return the
/// path. Throws std::runtime_error on mkstemp / write failure.
std::string write_temp_config(const std::string& body);

/// Thin RAII wrapper around an `ACE_Process`. Owns the child's pid,
/// remembers the path of the generated config (when applicable),
/// and ensures terminate() / wait() at destruction so a daemon-side
/// throw can't leave a zombie openvpn behind.
class OpenVpnProcess {
public:
    OpenVpnProcess();
    ~OpenVpnProcess();

    OpenVpnProcess(const OpenVpnProcess&)            = delete;
    OpenVpnProcess& operator=(const OpenVpnProcess&) = delete;

    /// Generic spawn — exec `executable` with `argv` (argv[0] is the
    /// program name the child will see; convention is the same string
    /// as `executable`). Used by tests with /bin/sh.
    /// Returns true on successful fork+exec.
    bool spawn(const std::string&              executable,
               const std::vector<std::string>& argv);

    /// Convenience: build the openvpn config, write it to a temp
    /// file, then spawn `openvpn_path` with `--config <tmp>
    /// --management 127.0.0.1 <mgmt_port>`. Records the temp path
    /// in config_path() so the caller (or destructor) can unlink it.
    bool spawn_openvpn(const OpenVpnConfig& cfg,
                       const std::string&   openvpn_path = "/usr/sbin/openvpn");

    /// Subprocess pid (0 if no spawn yet).
    pid_t pid() const { return m_pid; }

    /// Cheap liveness probe — non-blocking waitpid. Returns true if
    /// the child is still running. False on exit (caches the exit
    /// code so a subsequent wait() returns it without another waitpid).
    bool running();

    /// Block until the child exits. Returns the exit code (0..255),
    /// or -1 if killed by a signal. Idempotent: subsequent calls
    /// return the cached exit code.
    int wait();

    /// SIGTERM the child; if still alive after `grace`, SIGKILL.
    /// Reap via wait() so no zombie. No-op if there's no child.
    void terminate(std::chrono::milliseconds grace =
                       std::chrono::seconds(2));

    /// Path of the generated config when spawn_openvpn() was used;
    /// empty otherwise (raw spawn doesn't write a config).
    const std::string& config_path() const { return m_config_path; }

private:
    pid_t       m_pid = 0;
    int         m_exit_code = -1;
    bool        m_waited = false;
    std::string m_config_path;
};

} // namespace openvpn_client

#endif /* __openvpn_client_process_hpp__ */
