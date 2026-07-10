#ifndef __net_router_nft_rules_hpp__
#define __net_router_nft_rules_hpp__

/// Pure nftables ruleset generator.
///
/// No syscalls, no `nft` invocation, no logging — takes a State POD,
/// returns the script text that gets piped to `nft -f -` in D6.
/// Keeps unit tests table-driven; the apply step (privileged) is
/// orthogonal.
///
/// Generated ruleset uses table `inet iot_router` so it never
/// collides with NetworkManager / docker / fail2ban rules (each
/// nftables writer scopes by table name).

#include <cstdint>
#include <string>
#include <vector>

namespace net_router::nft {

/// Operator-supplied rule (parsed from `net.custom.rules` JSON).
struct CustomRule {
    std::string action;  ///< "forward" | "drop" | "accept"
    std::string proto;   ///< "tcp" | "udp"  (lowercase)
    std::uint32_t dport = 0;  ///< 0 = unspecified
    std::uint32_t sport = 0;
    std::string to_ip;        ///< only meaningful when action=="forward"
    std::uint32_t to_port = 0;
};

/// Inputs to the rule generator. Mirrors the read-key surface of
/// DsBridge plus the live tun IP observed at runtime. Plain POD so
/// the tests stay pure.
struct State {
    std::string tun_dev;                 ///< e.g. "tun0"
    std::string lwm2m_target_ip;         ///< required for any DNAT rule
    std::uint32_t lwm2m_target_port = 0; ///< informational; net.forward.ports is authoritative
    std::vector<std::uint16_t> forward_ports;
    std::vector<CustomRule>    custom;
};

/// Parse the comma-joined forward_ports string ("80,443,5684") into
/// a vector. Empty input → empty vector. Tokens that don't parse as
/// a u16 in [1,65535] are skipped (logged by the caller if it cares).
std::vector<std::uint16_t> parse_forward_ports(const std::string& csv);

/// Parse the operator-supplied `net.custom.rules` JSON-string into
/// a vector of CustomRule. Returns empty vector on parse error +
/// sets `*parse_error` (non-null) to the diagnostic. The daemon
/// keeps the previous ruleset live on parse failure.
std::vector<CustomRule>
parse_custom_rules(const std::string& json, std::string* parse_error = nullptr);

/// Render the full nft script text. Idempotent: starts with a create-if-absent
/// `add table inet iot_router` followed by `flush table inet iot_router`, so
/// re-applying never doubles and a missing table does not reject the ruleset
/// (`nft -f` is atomic).
std::string build_nft_ruleset(const State& s);

} // namespace net_router::nft

#endif /* __net_router_nft_rules_hpp__ */
