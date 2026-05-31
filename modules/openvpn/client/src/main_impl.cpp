#include "client.hpp"            // public v0 API (declares v0_dump_vpn_keys + run_daemon)

#include "ds_bridge.hpp"
#include "lifecycle.hpp"
#include "mgmt_protocol.hpp"
#include "process.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <ace/Log_Msg.h>
#include <ace/INET_Addr.h>
#include <ace/SOCK_Connector.h>
#include <ace/SOCK_Stream.h>
#include <ace/Time_Value.h>

namespace openvpn_client {

namespace {

const char* show(const std::optional<std::string>& v) {
    return v.has_value() ? v->c_str() : "(unset)";
}
const char* show(const std::optional<std::uint32_t>& v) {
    static thread_local char buf[24];
    if (!v.has_value()) return "(unset)";
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*v));
    return buf;
}

/// Snapshot DsBridge into an OpenVpnConfig POD. Assumes caller
/// already verified missing_required() is nullopt.
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

/// Block-connect to localhost:mgmt_port with 100 ms retries up to
/// total_ms timeout. openvpn takes ~50-500ms to come up + bind.
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

/// Read mgmt bytes + feed Parser + step Lifecycle. Returns when the
/// subprocess dies, the socket closes, or (in --once mode) the
/// first PUSH_REPLY has been seen.
void event_loop(ACE_SOCK_Stream&  stream,
                OpenVpnProcess&   proc,
                Lifecycle&        cli,
                mgmt::Parser&     parser,
                bool              once) {
    char buf[4096];
    while (proc.running()) {
        ACE_Time_Value t(0, 200 * 1000);
        ssize_t n = stream.recv(buf, sizeof(buf), &t);
        if (n > 0) {
            parser.feed(std::string_view(buf, n));
            while (auto ev = parser.next()) {
                cli.step(*ev);
            }
            if (once && cli.saw_push_reply()) return;
            continue;
        }
        if (n == 0) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt socket EOF\n")));
            return;
        }
        if (errno == ETIME || errno == ETIMEDOUT || errno == EAGAIN) continue;
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt recv errno=%d\n"),
                   errno));
        return;
    }
}

} // namespace

Status v0_dump_vpn_keys(const std::string& socketPath) {
    DsBridge ds(socketPath);
    if (!ds.connected()) {
        Status s; s.ok = false; s.code = 1;
        s.err = "data-store connect failed"; return s;
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l vpn.* snapshot from %C:\n"),
               ds.socket_path().c_str()));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.host  = %C\n"), show(ds.remote_host())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.port  = %C\n"), show(ds.remote_port())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.proto = %C\n"), show(ds.remote_proto())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.cert.path    = %C\n"), show(ds.cert_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.key.path     = %C\n"), show(ds.key_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.ca.path      = %C\n"), show(ds.ca_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.cipher       = %C\n"), show(ds.cipher())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.dev          = %C\n"), show(ds.dev())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.mgmt.port    = %C\n"), show(ds.mgmt_port())));
    if (auto missing = ds.missing_required()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < missing->size(); ++i) {
            if (i) joined << ", "; joined << (*missing)[i];
        }
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l required keys missing: %C\n"),
                   joined.str().c_str()));
    } else {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l all required keys present\n")));
    }
    return {};
}

Status run_daemon(const std::string& socketPath,
                  const std::string& openvpn_path,
                  bool               once) {
    DsBridge ds(socketPath);
    if (!ds.connected()) {
        Status s; s.ok = false; s.code = 1;
        s.err = "data-store connect failed"; return s;
    }
    if (auto missing = ds.missing_required()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < missing->size(); ++i) {
            if (i) joined << ", "; joined << (*missing)[i];
        }
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l refuse to start; "
                            "required keys missing: %C\n"),
                   joined.str().c_str()));
        Status s; s.ok = false; s.code = 2;
        s.err = "missing required vpn.* keys"; return s;
    }

    OpenVpnConfig cfg = snapshot_to_config(ds);
    OpenVpnProcess proc;
    if (!proc.spawn_openvpn(cfg, openvpn_path)) {
        ds.set_state("exited");
        Status s; s.ok = false; s.code = 3;
        s.err = "openvpn spawn failed"; return s;
    }
    ds.set_state("connecting");
    ds.set_pid(static_cast<std::uint32_t>(proc.pid()));
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l spawned openvpn pid=%d, "
                        "waiting on mgmt 127.0.0.1:%u\n"),
               static_cast<int>(proc.pid()),
               static_cast<unsigned>(cfg.mgmt_port)));

    ACE_SOCK_Stream stream;
    if (!connect_mgmt(static_cast<std::uint16_t>(cfg.mgmt_port), stream)) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l mgmt connect timed out\n")));
        proc.terminate();
        ds.set_state("exited");
        ds.set_exit_code(proc.wait());
        Status s; s.ok = false; s.code = 4;
        s.err = "mgmt connect timeout"; return s;
    }

    Lifecycle::Sinks sinks;
    sinks.set_state            = [&ds](const std::string& s) { ds.set_state(s); };
    sinks.set_assigned_ip      = [&ds](const std::string& s) { ds.set_assigned_ip(s); };
    sinks.set_assigned_gateway = [&ds](const std::string& s) { ds.set_assigned_gateway(s); };
    sinks.set_assigned_netmask = [&ds](const std::string& s) { ds.set_assigned_netmask(s); };
    sinks.set_assigned_dns     = [&ds](const std::string& s) { ds.set_assigned_dns(s); };
    sinks.on_first_push_reply  = []() {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l first PUSH_REPLY processed; "
                            "tunnel config in data-store\n")));
    };
    Lifecycle    cli(std::move(sinks));
    mgmt::Parser parser;
    event_loop(stream, proc, cli, parser, once);

    stream.close();
    if (proc.running()) proc.terminate();
    int code = proc.wait();
    ds.set_state("exited");
    ds.set_exit_code(code);
    return {};
}

} // namespace openvpn_client
