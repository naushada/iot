#ifndef __smsctl_executor_hpp__
#define __smsctl_executor_hpp__

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "smsctl/command.hpp"
#include "smsctl/session.hpp"

/**
 * @file executor.hpp
 * @brief Authenticates a parsed command and turns it into ds writes /
 *        privileged trigger files, returning the reply SMS text.
 *
 * Everything the executor touches goes through `DsSink`, so the whole engine
 * is unit-testable against a mock — no ds server and no modem in the tests.
 *
 * The daemon holds NO extra privilege: mutations are either ds writes (subject
 * to the normal ds ACLs) or a trigger file under /run/iot (2775 root:iot)
 * consumed by a root `.path` unit — the pattern iot-httpd already uses for
 * reboot / factory-reset.
 */

namespace smsctl {

/// Everything the executor needs from the outside world.
struct DsSink {
    virtual ~DsSink() = default;
    /// Persistent ds write. Returns false on failure (ACL, transport).
    virtual bool set(const std::string& key, const std::string& value) = 0;
    /// ds read; nullopt when the key is absent/unreadable.
    virtual std::optional<std::string> get(const std::string& key) = 0;
    /// Create a trigger file (the privilege bridge to a root .path unit).
    virtual bool arm_trigger(const std::string& path,
                             const std::string& content) = 0;
    /// Monotonic-ish token for the *.request envelopes (epoch millis).
    virtual std::uint64_t now_ms() = 0;
};

/// Trigger paths — the root `.path` units that watch them do the privileged
/// work. Used ONLY for reboot / factory-reset, which have no owning daemon:
/// the .path unit IS the module. Everything else is routed through ds to the
/// daemon that owns that domain (cellular-client for cell.*, wifi-client for
/// wifi.networks), which is the repo's command bus.
constexpr const char* kRebootTrigger       = "/run/iot/reboot.request";
constexpr const char* kFactoryResetTrigger = "/run/iot/factory-reset.request";

class Executor {
public:
    Executor(DsSink& sink, SessionStore& sessions, AccountLookup lookup)
      : m_sink(sink), m_sessions(sessions), m_lookup(std::move(lookup)) {}

    /// Authenticate + execute. Returns the reply SMS body (`OK …` / `ERR …`),
    /// already clamped to one GSM-7 SMS. Never contains a password or PSK.
    /// `now` is epoch seconds; `seed` feeds the factory-reset nonce.
    std::string handle(const Command& cmd, const std::string& sender,
                       std::uint64_t now, std::uint64_t seed);

    /// Longest reply we will send — one GSM-7 SMS.
    static constexpr std::size_t kMaxReply = 160;

private:
    std::string do_status();
    std::string do_apn(const std::string& apn);
    std::string do_radio_restart();
    std::string do_wifi(const std::string& ssid, const std::string& psk);
    std::string do_reboot();
    std::string do_factory_reset(const Command& cmd, const std::string& sender,
                                 std::uint64_t now, std::uint64_t seed);

    DsSink&       m_sink;
    SessionStore& m_sessions;
    AccountLookup m_lookup;
};

/// Upsert `{ssid, psk, key_mgmt}` into a `wifi.networks` JSON array string
/// (replace a same-SSID entry, else append). An empty psk yields an open
/// network (`key_mgmt:"NONE"`, no psk field). A malformed/empty input array is
/// tolerated — we start a fresh one rather than bricking WiFi config.
/// Exposed for unit tests.
std::string wifi_networks_upsert(const std::string& networks_json,
                                 const std::string& ssid,
                                 const std::string& psk);

/// Clamp a reply to one SMS (ellipsis when truncated). Exposed for tests.
std::string clamp_reply(std::string s);

} // namespace smsctl

#endif /* __smsctl_executor_hpp__ */
