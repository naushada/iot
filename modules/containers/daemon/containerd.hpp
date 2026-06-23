#ifndef __iot_containerd_hpp__
#define __iot_containerd_hpp__

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"

/// iot-containerd — privileged single-container runtime shim (crun-backed).
///
/// Phase 1 (this build) is the control-plane skeleton. It connects to the
/// data-store, publishes the container.* status keys (state=idle on boot) and
/// watches the container.{pull,run,stop}.request command keys the device-ui
/// bumps. Each command is acknowledged on the reactor thread; until the pull /
/// overlay-mount / crun phases land, the action is reported back as
/// not-yet-implemented via container.error. A `pull` does run the pure
/// containers::parse_image_ref() so the parsed registry/repo/tag already shows
/// up in the log. See apps/docs/tdd-device-containers.md.
///
/// Reactor-driven and ACE-only by project convention. The ds watch callback
/// fires on the client's listener thread; it only records the new request token
/// and notify()s the reactor, so all command handling runs single-threaded on
/// the reactor thread (handle_exception) — the home the future crun/mount work
/// needs.

namespace containers {

class Containerd : public ACE_Event_Handler {
public:
    struct Config {
        std::string ds_sock;                          ///< "" → ds default socket
        // Dedicated top-level paths — deliberately NOT under /var/lib/iot
        // (that is ds-server's DynamicUser StateDirectory=iot; pre-creating
        // children there makes systemd migrate it to /var/lib/private/iot).
        std::string root = "/var/lib/iot-containers";  ///< persistent image/layer store
        std::string run  = "/run/iot-containers";      ///< ephemeral overlay/bundle dir
    };

    explicit Containerd(Config cfg) : m_cfg(std::move(cfg)) {}
    ~Containerd() override;

    /// Connect ds, publish initial status, register the command watch, run the
    /// reactor. Returns a process exit code (non-zero on ds-connect/watch
    /// failure so systemd restarts us).
    int run();

    /// Reactor-thread command pump — woken by the ds watch via notify().
    int handle_exception(ACE_HANDLE = ACE_INVALID_HANDLE) override;

private:
    std::string get_str(const std::string& key);
    void publish_initial_status();
    void on_command_event(const data_store::Client::Event& ev);
    void handle_pull();
    void handle_run();
    void handle_stop();
    void set_state(const char* state);
    void set_error(const std::string& msg);

    Config             m_cfg;
    data_store::Client m_ds;

    // Request tokens last seen + a per-command pending flag. The tokens are
    // baselined at startup so a stale value (a previous session's request) does
    // not fire on boot. Written on the listener thread (on_command_event),
    // drained on the reactor thread (handle_exception) — guarded by m_mtx.
    std::mutex  m_mtx;
    std::string m_pull_token;
    std::string m_run_token;
    std::string m_stop_token;
    bool        m_pull_pending = false;
    bool        m_run_pending  = false;
    bool        m_stop_pending = false;

    // Pull runs on a worker thread (a registry pull can take minutes) so the
    // reactor stays responsive; the ds Client is thread-safe, so the worker
    // publishes container.* progress/state directly.
    std::thread       m_pull_thread;
    std::atomic<bool> m_pull_active{false};
    std::atomic<bool> m_pull_cancel{false};

    // Run (layer extract + overlay mount + crun create/start + supervision)
    // runs on a worker thread that lives for the container's lifetime.
    std::thread       m_run_thread;
    std::atomic<bool> m_run_active{false};
    std::atomic<bool> m_stop_requested{false};   ///< set by handle_stop, honored by the run worker
    std::atomic<bool> m_shutdown{false};         ///< daemon teardown — supervise loop exits fast (SIGKILL)
    std::string       m_merged;   ///< mounted rootfs path (guarded by m_mtx)
};

} // namespace containers

#endif /* __iot_containerd_hpp__ */
