#ifndef __iot_containerd_hpp__
#define __iot_containerd_hpp__

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"

/// iot-containerd — privileged multi-container runtime shim (crun-backed).
///
/// Phase 2 runs any number of independently-named containers. The ds schema is
/// static (no per-name keys), so the set of containers is carried as ONE JSON
/// document (`container.instances`) the daemon publishes and the device-ui grid
/// renders, and commands arrive through a single envelope (`container.cmd.*`)
/// routed by name. See apps/docs/tdd-device-containers.md §13.
///
/// Reactor-driven and ACE-only by convention. The ds watch callback fires on the
/// client's listener thread; it only records the new command token and notify()s
/// the reactor, so all command dispatch runs single-threaded on the reactor
/// thread (handle_exception). Each running container has its own supervise
/// thread; the registry pull is serialised (one at a time — the long pole).
/// m_mtx guards the container map, every mutable per-instance field, the IP
/// allocator, and the command token.

namespace containers {

/// One named container: its config, pulled-image handle, live runtime state, and
/// (when running) a supervise thread. Non-copyable/movable (owns a thread +
/// atomics) — held by unique_ptr in the daemon's map.
struct ContainerInstance {
    explicit ContainerInstance(std::string nm, std::string i)
        : name(std::move(nm)), id(std::move(i)) {}

    // identity
    std::string name;                 ///< operator-chosen name (map key)
    std::string id;                   ///< crun id / path component ("c-"+sanitized name)

    // config (set on pull/run)
    std::string image_ref;
    std::string net_mode = "host";    ///< "host" | "bridge"
    std::string net_subnet;
    std::string mem, cpus;
    std::string entrypoint, cmd;

    // pulled image
    std::string image_id;             ///< config digest ("sha256:…")
    std::string image_size;           ///< total bytes (decimal)

    // live state
    std::string state = "idle";       ///< idle|pulling|pulled|mounting|created|running|stopped|error
    std::string error;
    std::string ip, gateway;          ///< bridge mode
    int         host_octet = 0;       ///< allocated bridge octet (.N); 0 = none
    long        pid = 0;
    int         exit_code = -1;       ///< -1 = none
    long long   started = 0;

    // live cgroup usage (sampled by the supervise loop while running; cleared on
    // exit). Formatted for direct display — "12.3M" / "3.2%"; empty ⇒ "—".
    std::string mem_usage;            ///< memory.current, human bytes
    std::string cpu_pct;              ///< cpu.stat usage delta over wall time

    // runtime
    std::string       merged;         ///< mounted rootfs path
    std::thread       run_thread;
    std::atomic<bool> run_active{false};
    std::atomic<bool> stop_requested{false};
};

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

    void on_command_event(const data_store::Client::Event& ev);
    void handle_command();                           // dispatch one envelope

    void do_pull(ContainerInstance* inst);
    void do_run(ContainerInstance* inst);
    void do_stop(ContainerInstance* inst);
    void do_remove(const std::string& name);
    // Delete image content (manifests/blobs/extracted layers) no live container
    // references — reclaims disk after a re-pull or remove. Skipped while a pull
    // is in flight. Safe: shared layers are kept (see image_prune planner).
    void do_prune();
    // The supervise body of a running container (own thread). Args by value so
    // the thread owns its copies; `inst` stays valid because do_remove refuses to
    // erase a container while its run_active flag is set.
    void run_worker(ContainerInstance* inst, std::string image_id,
                    std::string root, std::string run_root,
                    std::string ov_entrypoint, std::string ov_cmd,
                    std::string lim_mem, std::string lim_cpus,
                    bool bridge, std::string net_subnet, std::string id, int octet);
    // Wait for a running container to exit (or a stop request), sampling live
    // cgroup usage, then tear it down (crun delete + bridge/overlay unwind) and
    // publish the final stopped state. Shared by run_worker (own_child=true —
    // waitpid captures the real exit code) and rehydrate (own_child=false — the
    // container reparented to init while we were down, so we poll crun state and
    // the exit code reads as unknown). Clears inst->run_active on return.
    void supervise(ContainerInstance* inst, std::string id, std::string run_root,
                   bool bridge, long init_pid, bool own_child);

    // Startup reconcile: rebuild the container map from the data-store's
    // `container.instances` roster (the source of truth — we keep no on-disk
    // state) and reconcile each entry against live crun state, re-attaching a
    // supervisor to any container that kept running while the daemon was down.
    void rehydrate_from_ds();

    // m_mtx MUST be held for these.
    ContainerInstance* find_locked(const std::string& name);
    ContainerInstance* ensure_locked(const std::string& name);
    int  alloc_octet_locked();                       // lowest free .N (>=2), 0 if full
    void free_octet_locked(int octet);
    void publish_instances_locked();                 // serialise map → container.instances

    Config             m_cfg;
    data_store::Client m_ds;

    std::mutex m_mtx;
    std::map<std::string, std::unique_ptr<ContainerInstance>> m_containers;
    std::set<int> m_used_octets;                     // bridge IPs in use (.2/.3/…)

    // Single command envelope, baselined at startup so a stale token does not
    // fire on boot. Written on the listener thread, drained on the reactor.
    std::string m_cmd_token;
    bool        m_cmd_pending = false;

    // Registry pull is serialised (one worker). m_pull_name is the target.
    std::thread       m_pull_thread;
    std::atomic<bool> m_pull_active{false};
    std::atomic<bool> m_pull_cancel{false};
    std::string       m_pull_name;                   // guarded by m_mtx

    std::atomic<bool> m_shutdown{false};             // teardown → supervise loops exit fast
};

} // namespace containers

#endif /* __iot_containerd_hpp__ */
