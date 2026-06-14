#include "supervisor.hpp"

#include "data_store/service_gate.hpp"
#include "data_store/dep_watch.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <ace/Log_Msg.h>
#include <ace/INET_Addr.h>
#include <ace/SOCK_Connector.h>
#include <ace/SOCK_Stream.h>
#include <ace/Time_Value.h>

#include "lifecycle.hpp"
#include "mgmt_protocol.hpp"
#include "process.hpp"

namespace openvpn_client {

namespace {

/// Snapshot DsBridge into the OpenVpnConfig POD. Mirrors what the
/// old run_daemon did inline; callers must have already verified
/// missing_required() returns nullopt before invoking.
OpenVpnConfig snapshot_to_config(const DsBridge& ds) {
    OpenVpnConfig c;
    c.remote_host  = ds.remote_host().value_or("");
    c.remote_port  = ds.remote_port().value_or(1194);
    c.remote_proto = ds.remote_proto().value_or("udp");
    c.cert_path    = ds.cert_path().value_or("");
    c.key_path     = ds.key_path().value_or("");
    c.ca_path      = ds.ca_path().value_or("");
    c.cipher       = ds.cipher().value_or("AES-256-GCM");
    c.dev          = ds.dev().value_or("tun");
    c.mgmt_port    = ds.mgmt_port().value_or(7505);
    return c;
}

/// Block-connect to localhost:port with 100 ms retries up to total_ms.
/// Lifted verbatim from the old main_impl.cpp connect_mgmt() — same
/// behaviour, same 5 s default deadline.
bool connect_mgmt(std::uint16_t port,
                  ACE_SOCK_Stream& stream,
                  int total_ms = 5000) {
    ACE_INET_Addr addr(port, "127.0.0.1");
    ACE_SOCK_Connector conn;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(total_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        ACE_Time_Value t(0, 250 * 1000);
        if (conn.connect(stream, addr, &t) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

Supervisor::Supervisor(DsBridge& ds, Options opt)
  : m_ds(ds), m_opt(std::move(opt)) {
    // L16/D4 — services.openvpn.client.enable gate. We construct it
    // lazily here (DsBridge already owns the Client + listener) and
    // spawn a tiny watcher thread that forwards gate transitions
    // into m_cv so the existing wait_for_event() handles both WAN
    // events AND enable flips through the same code path.
    if (auto* cli = m_ds.client()) {
        m_svc = std::make_unique<data_store::ServiceGate>(*cli,
                                                          "openvpn.client");
        m_svc_watcher = std::thread([this] {
            for (;;) {
                auto v = m_svc->wait();
                if (!v.has_value()) return;     // shutdown
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_svc_dirty.store(true);
                    if (m_shutdown) return;
                }
                m_cv.notify_all();
            }
        });

        // L17a/D3 — dependency watch. Construct DepWatch with the
        // declared dependencies from services.lua (v1: just
        // net.router). The dep watcher thread forwards dep health
        // transitions into m_cv so wait_for_event() handles dep,
        // svc, and WAN events through one cv.
        m_dep = std::make_unique<data_store::DepWatch>(
            *cli, std::vector<std::string>{"net.router"});
        m_dep_watcher = std::thread([this] {
            for (;;) {
                if (!m_dep->wait()) return;     // shutdown
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_dep_dirty.store(true);
                    if (m_shutdown) return;
                }
                m_cv.notify_all();
            }
        });
    }
}

Supervisor::~Supervisor() {
    {
        std::lock_guard<std::mutex> g(m_mtx);
        m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_svc) m_svc->shutdown();
    if (m_dep) m_dep->shutdown();
    if (m_svc_watcher.joinable()) m_svc_watcher.join();
    if (m_dep_watcher.joinable()) m_dep_watcher.join();
}

void Supervisor::on_wan_event(const std::optional<std::string>& target) {
    {
        std::lock_guard<std::mutex> g(m_mtx);
        m_pending_target = target;
        m_wan_dirty      = true;
    }
    m_cv.notify_all();
}

std::optional<std::string> Supervisor::drain_target() {
    std::lock_guard<std::mutex> g(m_mtx);
    m_wan_dirty = false;
    return m_pending_target;
}

void Supervisor::on_config_event() {
    m_cfg_dirty.store(true, std::memory_order_release);
    m_cv.notify_all();
}

bool Supervisor::cfg_dirty() {
    return m_cfg_dirty.load(std::memory_order_acquire);
}

bool Supervisor::wait_for_event() {
    std::unique_lock<std::mutex> g(m_mtx);
    m_cv.wait(g, [&]{
        return m_shutdown
            || m_wan_dirty
            || m_svc_dirty.load(std::memory_order_acquire)
            || m_dep_dirty.load(std::memory_order_acquire)
            || m_cfg_dirty.load(std::memory_order_acquire);
    });
    return !m_shutdown;
}

bool Supervisor::wan_dirty() {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_wan_dirty;
}

bool Supervisor::serve_one_session(const std::string& iface) {
    OpenVpnConfig cfg = snapshot_to_config(m_ds);
    OpenVpnProcess proc;
    if (!proc.spawn_openvpn(cfg, m_opt.openvpn_path)) {
        m_ds.set_state("exited");
        return false;
    }
    m_ds.set_state("connecting");
    m_ds.set_pid(static_cast<std::uint32_t>(proc.pid()));
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l spawned openvpn pid=%d on "
                        "iface=%C, waiting on mgmt 127.0.0.1:%u\n"),
               static_cast<int>(proc.pid()),
               iface.c_str(),
               static_cast<unsigned>(cfg.mgmt_port)));

    ACE_SOCK_Stream stream;
    if (!connect_mgmt(static_cast<std::uint16_t>(cfg.mgmt_port), stream)) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt connect timed out\n")));
        proc.terminate();
        m_ds.set_state("exited");
        m_ds.set_exit_code(proc.wait());
        return false;
    }

    // Subscribe to real-time state notifications. Real openvpn keeps the
    // management interface silent after the `>INFO:` banner until a client
    // asks for them. `state on` enables FUTURE `>STATE:` transitions, but it
    // does NOT replay the current state — so if openvpn already reached
    // CONNECTED before we attached the mgmt socket (the common race), that
    // event is missed and vpn.state stalls at "connecting" forever (even
    // though PUSH_REPLY still sets vpn.assigned.*). Fix: also query `state 1`
    // for the most-recent state right now. Its reply is a bare comma-record
    // (no `>STATE:` prefix) terminated by END; the mgmt Parser surfaces that
    // as a State event too, so the Lifecycle advances vpn.state to "connected".
    {
        static const char kStateOn[] = "state on\r\n";
        if (stream.send_n(kStateOn, sizeof(kStateOn) - 1) < 0) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt 'state on' send "
                                "failed errno=%d; vpn.state may stall\n"),
                       errno));
        }
        static const char kStateQuery[] = "state 1\r\n";
        if (stream.send_n(kStateQuery, sizeof(kStateQuery) - 1) < 0) {
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt 'state 1' query "
                                "failed errno=%d; vpn.state may stall at "
                                "connecting\n"),
                       errno));
        }
        // openvpn was spawned with `management-hold`, so it is paused until we
        // release it. Now that real-time state notifications are enabled, let
        // it connect — the CONNECTED state + PUSH_REPLY (assigned
        // ip/netmask/gateway/dns) then arrive while we're already listening,
        // so vpn.assigned.* is captured rather than raced.
        static const char kHoldRelease[] = "hold release\r\n";
        if (stream.send_n(kHoldRelease, sizeof(kHoldRelease) - 1) < 0) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt 'hold release' "
                                "failed errno=%d; openvpn will stay held and "
                                "never connect\n"),
                       errno));
        }
    }

    Lifecycle::Sinks sinks;
    sinks.set_state            = [this](const std::string& s) { m_ds.set_state(s); };
    sinks.set_assigned_ip      = [this](const std::string& s) { m_ds.set_assigned_ip(s); };
    sinks.set_assigned_gateway = [this](const std::string& s) { m_ds.set_assigned_gateway(s); };
    sinks.set_assigned_netmask = [this](const std::string& s) { m_ds.set_assigned_netmask(s); };
    sinks.set_assigned_dns     = [this](const std::string& s) { m_ds.set_assigned_dns(s); };
    sinks.on_first_push_reply  = []() {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l first PUSH_REPLY processed; "
                            "tunnel config in data-store\n")));
    };
    Lifecycle    cli(std::move(sinks));
    mgmt::Parser parser;

    // Event loop — recv with a 200 ms timeout, feed Parser, step
    // Lifecycle. Exit on:
    //   - subprocess death
    //   - mgmt EOF / hard error
    //   - WAN event (drain_target wakes us; we surface that via wan_dirty)
    //   - shutdown
    //   - --once mode after first PUSH_REPLY
    char buf[4096];
    while (proc.running()) {
        if (wan_dirty()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l WAN event during "
                                "session on iface=%C; tearing down\n"),
                       iface.c_str()));
            break;
        }
        // L17a/D3 — dep dominates: if a dependency goes unhealthy
        // mid-session, tear down within one event-loop tick.
        if (m_dep && !m_dep->healthy()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l dep %C unhealthy "
                                "during session on iface=%C; tearing down\n"),
                       m_dep->unhealthy_dep().c_str(), iface.c_str()));
            break;
        }
        // L16/D4 — gate dominates: if operator disables mid-session,
        // tear down within one event-loop tick (NFR-SVC-001).
        if (m_svc && !m_svc->enabled()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l services."
                                "openvpn.client.enable=false during "
                                "session on iface=%C; tearing down\n"),
                       iface.c_str()));
            break;
        }
        // Live config hot-reload: a vpn.* key changed (e.g. cloud pushed a new
        // vpn.remote.host). Tear down; the outer loop respawns openvpn with a
        // freshly snapshotted config.
        if (cfg_dirty()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l vpn config changed during "
                                "session on iface=%C; respawning with fresh "
                                "config\n"),
                       iface.c_str()));
            break;
        }
        {
            std::lock_guard<std::mutex> g(m_mtx);
            if (m_shutdown) break;
        }
        ACE_Time_Value t(0, 200 * 1000);
        ssize_t n = stream.recv(buf, sizeof(buf), &t);
        if (n > 0) {
            parser.feed(std::string_view(buf, n));
            while (auto ev = parser.next()) {
                // openvpn was spawned with `management-hold`, so it re-enters
                // HOLD before EVERY (re)connection attempt — not just at
                // startup. We release once at attach (above), but on a cloud
                // VPN-server restart / link drop it holds again and emits a
                // fresh >HOLD:. Without re-releasing, the client wedges in hold
                // forever (device shows "reconnecting" but never reconnects).
                // Re-release on each HOLD so reconnects actually proceed.
                if (ev->kind == mgmt::Event::Kind::Hold) {
                    static const char kHoldRelease[] = "hold release\r\n";
                    if (stream.send_n(kHoldRelease, sizeof(kHoldRelease) - 1) < 0) {
                        ACE_ERROR((LM_WARNING,
                                   ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt re-'hold "
                                            "release' on reconnect failed "
                                            "errno=%d\n"), errno));
                    } else {
                        ACE_DEBUG((LM_INFO,
                                   ACE_TEXT("%D [ovpn:%t] %M %N:%l openvpn "
                                            "re-entered HOLD (server restart/link "
                                            "drop); released to reconnect\n")));
                    }
                }
                cli.step(*ev);
            }
            if (m_opt.once && cli.saw_push_reply()) break;
            continue;
        }
        if (n == 0) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt socket EOF\n")));
            break;
        }
        if (errno == ETIME || errno == ETIMEDOUT || errno == EAGAIN) continue;
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt recv errno=%d\n"),
                   errno));
        break;
    }

    stream.close();
    if (proc.running()) proc.terminate();
    int code = proc.wait();
    m_ds.set_state("exited");
    m_ds.set_exit_code(code);
    return true;
}

Status Supervisor::run() {
    m_ds.on_wan_change([this](auto t) { this->on_wan_event(t); });
    // Live hot-reload: any vpn.* config change tears the session down and
    // respawns openvpn with the regenerated config (e.g. a cloud-pushed
    // vpn.remote.host). DsBridge already updated its cache before this fires.
    m_ds.on_change([this](DsBridge::Key) { this->on_config_event(); });

    // Initial publish: until proven otherwise we are gated.
    m_ds.set_state("disconnected");
    m_ds.set_gate_reason("wan_down");
    m_ds.set_bound_iface("");
    if (m_svc) m_svc->publish_state("running");

    // Prime: the snapshot may already hold an iface (net-router was
    // running before us). Treat it as the first event.
    on_wan_event(m_ds.wan_iface());

    while (true) {
        if (!wait_for_event()) break;          // shutdown

        // L17a/D3 composition (highest priority):
        // dep_down > disabled > wan_down.
        // If a dependency is unhealthy, park immediately regardless
        // of the svc gate or WAN state.
        m_dep_dirty.store(false, std::memory_order_release);
        m_svc_dirty.store(false, std::memory_order_release);
        // Cleared before (re)evaluating gates + respawning; a config change
        // arriving during the new session re-sets it for the next iteration.
        m_cfg_dirty.store(false, std::memory_order_release);
        if (m_dep && !m_dep->healthy()) {
            if (m_gate.running()) m_gate.note_terminated();
            m_ds.set_state("disabled");
            m_ds.set_gate_reason("dep_down:" + m_dep->unhealthy_dep());
            m_ds.set_bound_iface("");
            if (m_svc) m_svc->publish_state("disabled");
            continue;
        }

        // L16/D4 composition: enable=false dominates WAN. Even if
        // the WAN is up, do not spawn openvpn; park at
        // gate.reason="disabled" until the gate flips true.
        if (m_svc && !m_svc->enabled()) {
            // serve_one_session reaped its own child on the way
            // out; the Gate is in note_terminated state if we got
            // here mid-session. Reflect the disabled state.
            if (m_gate.running()) m_gate.note_terminated();
            m_ds.set_state("disabled");
            m_ds.set_gate_reason("disabled");
            m_ds.set_bound_iface("");
            m_svc->publish_state("disabled");
            continue;
        }
        // Gate is open. If we just came back from a disabled state,
        // publish state="starting" before the WAN evaluation below.
        if (m_svc) m_svc->publish_state("running");

        auto target = drain_target();
        const bool target_up = target.has_value() && !target->empty();

        GateDecision decision = m_gate.evaluate(target);

        if (!target_up) {
            // Either we're already idle (Action::None) or we just
            // tore the session down inside serve_one_session because
            // the WAN went away (Action::Terminate; the child is
            // already reaped). Publish + wait for the next event.
            if (decision.action == GateDecision::Action::Terminate) {
                m_gate.note_terminated();
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D [ovpn:%t] %M %N:%l WAN down "
                                    "(from=%C); session torn down\n"),
                           decision.from.c_str()));
            }
            m_ds.set_state("disconnected");
            m_ds.set_gate_reason("wan_down");
            m_ds.set_bound_iface("");
            continue;
        }

        // Target up. None (we're already running on this iface AND
        // the child is still healthy) only happens if serve_one_session
        // returned for non-WAN reasons while we were inside it — but
        // by construction serve_one_session reaps the child before
        // returning, so the Gate's note_terminated has NOT been called
        // yet. Defensive: any time we get here with target up, make
        // sure the gate reflects "not running" before deciding.
        if (m_gate.running()) {
            // Session ended (child died or WAN changed); reflect that.
            m_gate.note_terminated();
            decision = m_gate.evaluate(target);
        }

        if (decision.action == GateDecision::Action::Restart) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l WAN iface changed "
                                "%C → %C; restarting session\n"),
                       decision.from.c_str(),
                       decision.iface.c_str()));
        }

        m_ds.set_gate_reason("ok");
        m_ds.set_bound_iface(*target);
        m_gate.note_spawned(*target);

        const bool spawned = serve_one_session(*target);
        m_gate.note_terminated();
        m_ds.set_bound_iface("");
        if (!spawned) {
            m_ds.set_gate_reason("spawn_failed");
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l openvpn spawn "
                                "failed on iface=%C; waiting for next "
                                "WAN event\n"),
                       target->c_str()));
        }

        if (m_opt.once) break;
    }
    return {};
}

} // namespace openvpn_client
