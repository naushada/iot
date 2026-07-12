#ifndef __smsctl_command_hpp__
#define __smsctl_command_hpp__

#include <string>
#include <vector>

/**
 * @file command.hpp
 * @brief The parsed form of an inbound `IOT …` control SMS.
 *
 * The grammar is deliberately human-typable from any phone (see
 * apps/docs/tdd-smsctl.md): a mandatory `IOT ` prefix, a case-insensitive
 * keyword, and space-separated arguments that may be double-quoted when they
 * contain spaces (SSIDs and passwords routinely do).
 */

namespace smsctl {

enum class Kind {
    NotACommand,    ///< no `IOT ` prefix — ordinary text / carrier spam; drop silently
    Unknown,        ///< `IOT <junk>` — the sender meant a command but got it wrong
    Login,          ///< args: user, password
    Logout,
    Status,
    Reboot,
    FactoryReset,   ///< args: [nonce]
    Apn,            ///< args: apn
    RadioRestart,
    Wifi,           ///< args: ssid, [psk]
};

struct Command {
    Kind                     kind = Kind::NotACommand;
    std::vector<std::string> args;
    /// Human-readable parse error for `Unknown` (never contains argument
    /// values — a password/PSK must not leak into a reply, ds or the log).
    std::string              error;

    /// Keyword as it is echoed back in `OK <CMD>` / `ERR <CMD>` replies and
    /// published to `smsctl.last.cmd`. Never includes arguments.
    const char* keyword() const;
};

/// Whether this command mutates the device (→ Admin access required).
/// STATUS/LOGIN/LOGOUT are non-mutating.
bool is_mutating(Kind k);

} // namespace smsctl

#endif /* __smsctl_command_hpp__ */
