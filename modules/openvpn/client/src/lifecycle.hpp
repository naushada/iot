#ifndef __openvpn_client_lifecycle_hpp__
#define __openvpn_client_lifecycle_hpp__

/// Lifecycle FSM that maps openvpn mgmt-interface events to
/// vpn.* writes. Pure logic — takes Sinks (callable handles) so
/// tests substitute fakes without standing up a real DsBridge +
/// ds-server.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mgmt_protocol.hpp"

namespace openvpn_client {

class Lifecycle {
public:
    /// Per-key writers — production builds these from a DsBridge&;
    /// tests substitute lambdas that capture into a struct.
    struct Sinks {
        std::function<void(const std::string&)>   set_state;
        std::function<void(const std::string&)>   set_assigned_ip;
        std::function<void(const std::string&)>   set_assigned_gateway;
        std::function<void(const std::string&)>   set_assigned_netmask;
        std::function<void(const std::string&)>   set_assigned_dns;
        std::function<void()>                     on_first_push_reply;
    };

    explicit Lifecycle(Sinks sinks) : m_sinks(std::move(sinks)) {}

    /// Consume one parsed mgmt event. Writes to sinks on any
    /// state transition or push-option change.
    void step(const mgmt::Event& ev);

    bool saw_push_reply() const { return m_saw_push_reply; }

private:
    Sinks m_sinks;
    bool  m_saw_push_reply = false;

    /// Map openvpn STATE field[1] (e.g. "CONNECTING") to the
    /// schema's vpn.state value (lowercase). Unknown STATE codes
    /// pass through verbatim.
    static std::string normalise_state(const std::string& openvpn_state);

    /// Apply a list of PUSH_REPLY options (each "name value..."), dispatching
    /// the known ones (ifconfig → ip+netmask, route-gateway, dhcp-option DNS) to
    /// the sinks. Shared by the >PUSH_REPLY event path and the >LOG "PUSH:
    /// Received control message: 'PUSH_REPLY,...'" path (real openvpn surfaces
    /// the pushed config via the log, not a PUSH_REPLY mgmt notification).
    void apply_push_options(const std::vector<std::string>& opts);
};

} // namespace openvpn_client

#endif /* __openvpn_client_lifecycle_hpp__ */
