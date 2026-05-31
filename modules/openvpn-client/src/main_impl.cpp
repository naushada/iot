#include "openvpn_client/client.hpp"

#include "ds_bridge.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include <ace/Log_Msg.h>

namespace openvpn_client {

namespace {

/// Pretty-print an optional<string> for the snapshot dump.
const char* show(const std::optional<std::string>& v) {
    return v.has_value() ? v->c_str() : "(unset)";
}

/// Same idea for an optional<uint32_t> — into a thread-local buffer
/// so the format spec stays %C (no %u juggling) and the caller can
/// keep one dispatch path.
const char* show(const std::optional<std::uint32_t>& v) {
    static thread_local char buf[24];
    if (!v.has_value()) return "(unset)";
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*v));
    return buf;
}

} // namespace

Status v0_dump_vpn_keys(const std::string& socketPath) {
    // D3 upgrade: replace the bare Client::get with a DsBridge, which
    // primes the snapshot + registers a watch (so any concurrent
    // ds-cli mutation surfaces while the dump is in flight — handy
    // for live operator inspection).
    DsBridge ds(socketPath);
    if (!ds.connected()) {
        // DsBridge already logged the connect-failure reason; just
        // surface a Status the caller can return as a non-zero exit.
        Status s;
        s.ok   = false;
        s.code = 1;
        s.err  = "data-store connect failed";
        return s;
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l vpn.* snapshot from %C:\n"),
               ds.socket_path().c_str()));

    // Read keys — accessor-by-accessor so the variant unwrap (and
    // schema-default application) happens in one place per key.
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.host  = %C\n"), show(ds.remote_host())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.port  = %C\n"), show(ds.remote_port())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.remote.proto = %C\n"), show(ds.remote_proto())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.cert.path    = %C\n"), show(ds.cert_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.key.path     = %C\n"), show(ds.key_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.ca.path      = %C\n"), show(ds.ca_path())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.cipher       = %C\n"), show(ds.cipher())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.dev          = %C\n"), show(ds.dev())));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ovpn:%t] %M %N:%l   vpn.mgmt.port    = %C\n"), show(ds.mgmt_port())));

    // Required-key gate: D6's lifecycle will refuse to spawn openvpn
    // here. For v0 we just log so operators can see which keys are
    // still missing as they bring up a fresh deployment.
    if (auto missing = ds.missing_required()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < missing->size(); ++i) {
            if (i) joined << ", ";
            joined << (*missing)[i];
        }
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l required keys missing: "
                            "%C — daemon mode (D6) will refuse to start\n"),
                   joined.str().c_str()));
    } else {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l all required keys "
                            "present; ready for D6 lifecycle\n")));
    }

    return {};
}

} // namespace openvpn_client
