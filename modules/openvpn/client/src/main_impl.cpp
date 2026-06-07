#include "client.hpp"            // public v0 API (declares v0_dump_vpn_keys + run_daemon)

#include "ds_bridge.hpp"
#include "supervisor.hpp"

#include "data_store/client.hpp"
#include "data_store/stats_publisher.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include <ace/Log_Msg.h>

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
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   net.iface.active = %C\n"), show(ds.wan_iface())));
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

    // L22 — resource telemetry. Blocking supervisor loop (no ACE reactor),
    // so StatsPublisher spawns its own ACE_Task thread for the singleton
    // reactor timer (run_reactor_thread=true, the default). openvpn(8) runs
    // in this container, so its usage is folded into these cgroup totals.
    data_store::StatsPublisher stats(
        "services.openvpn.client",
        [&ds](const std::vector<data_store::KV>& kv) {
            if (auto* c = ds.client()) c->set(kv);
        });
    if (ds.client()) stats.open();

    Supervisor::Options opt;
    opt.openvpn_path = openvpn_path;
    opt.once         = once;
    Supervisor sup(ds, std::move(opt));
    return sup.run();
}

} // namespace openvpn_client
