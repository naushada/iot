#ifndef __openvpn_client_supervisor_hpp__
#define __openvpn_client_supervisor_hpp__

/// WAN-gated supervisor for the openvpn child process.
///
/// L13 net-router publishes `net.iface.active` — the highest-priority
/// OPER-UP iface (eth0 / wlan0 / wwan0). The Supervisor subscribes to
/// that key via DsBridge::on_wan_change and uses it to decide whether
/// to run openvpn at all:
///
///   - WAN down (key unset or empty)  → no child, vpn.gate.reason=wan_down
///   - WAN up                          → spawn openvpn, drive Lifecycle
///   - WAN iface changed (eth0→wlan0)  → terminate + respawn so the new
///                                       default route is picked up
///
/// The watch callback fires on the data_store::Client listener thread;
/// the supervisor uses a condition_variable to forward those events
/// into the main thread that runs the mgmt event-loop. The event-loop's
/// 250 ms recv timeout doubles as the WAN-event polling interval.

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "client.hpp"
#include "ds_bridge.hpp"
#include "gate.hpp"

namespace data_store { class ServiceGate; class DepWatch; }   // forward decl

namespace openvpn_client {

class Supervisor {
public:
    struct Options {
        std::string openvpn_path = "/usr/sbin/openvpn";
        /// `--once`: exit cleanly after the first session reaches its
        /// first PUSH_REPLY. Used by smoke harnesses. WAN-down before
        /// any session ever starts still blocks indefinitely (the smoke
        /// is expected to seed net.iface.active first).
        bool        once         = false;
    };

    Supervisor(DsBridge& ds, Options opt);
    ~Supervisor();

    Supervisor(const Supervisor&)            = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    /// Block on the calling thread, driving the gated lifecycle until
    /// the process is killed. Caller-side signal handling is FUP —
    /// today SIGTERM kills the daemon; the OpenVpnProcess destructor
    /// reaps any running child.
    Status run();

private:
    DsBridge& m_ds;
    Options   m_opt;
    Gate      m_gate;

    /// L16/D4 — services.openvpn.client.enable gate. Composes with
    /// the WAN gate; the composition rule is "disable dominates":
    /// when m_svc->enabled() is false, the WAN target is ignored and
    /// the Supervisor parks at gate.reason="disabled".
    std::unique_ptr<data_store::ServiceGate> m_svc;
    std::thread                              m_svc_watcher;
    std::atomic<bool>                        m_svc_dirty{false};

    /// L17a/D3 — dependency watch. If any declared dependency (e.g.
    /// net.router) goes unhealthy, gate.reason="dep_down:<name>"
    /// dominates both the enable gate and WAN.
    std::unique_ptr<data_store::DepWatch>    m_dep;
    std::thread                              m_dep_watcher;
    std::atomic<bool>                        m_dep_dirty{false};

    std::mutex                 m_mtx;
    std::condition_variable    m_cv;
    std::optional<std::string> m_pending_target;  ///< latest WAN snapshot
    bool                       m_wan_dirty = false;
    bool                       m_shutdown  = false;

    /// Called from the DsBridge listener thread.
    void on_wan_event(const std::optional<std::string>& target);

    /// Snapshot the latest WAN target under lock, clearing the dirty
    /// flag. Returns nullopt on shutdown OR when target is empty.
    std::optional<std::string> drain_target();

    /// Block until a WAN event arrives or shutdown is requested.
    /// Returns true if a WAN event is now pending, false on shutdown.
    bool wait_for_event();

    /// Cheap snapshot of m_wan_dirty for the inner event-loop. No
    /// reset — the outer loop calls drain_target() to reset.
    bool wan_dirty();

    /// Spawn openvpn + connect mgmt + drive Lifecycle until the child
    /// dies, a WAN event arrives, or shutdown. Returns true on a
    /// clean spawn (false means spawn or mgmt-connect failed and the
    /// caller should publish gate.reason=spawn_failed).
    bool serve_one_session(const std::string& iface);
};

} // namespace openvpn_client

#endif /* __openvpn_client_supervisor_hpp__ */
