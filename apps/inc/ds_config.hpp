#ifndef __apps_ds_config_hpp__
#define __apps_ds_config_hpp__

/// Optional config-plane reader backed by the data-store unix-socket
/// service.
///
/// The iot binary tries to connect to ds-server (default `/run/ds.sock`,
/// overridable via `ds-sock=PATH` on the CLI). When the connection
/// succeeds the reader serves `iot.endpoint` / `iot.server.uri` /
/// `iot.lifetime` from the live store; when it fails (server not
/// running, socket missing, permission denied) every accessor returns
/// nullopt and the caller falls back to its compiled-in / lua_config
/// defaults. ds-server is strictly opt-in — its absence MUST NOT break
/// startup.
///
/// All accessors return `std::optional<T>` so the caller can use:
///   const std::string endpoint =
///       ds.endpoint().value_or(cli_arg_endpoint);
///   const std::uint32_t lifetime =
///       ds.lifetime().value_or(86400);
///
/// Threading: not internally synchronised. Construct on the main
/// thread before fan-out; the underlying data_store::Client owns its
/// own listener thread once connect() runs.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace iot {

class DsConfig {
public:
    /// Connect to `socketPath` (or kDefault when empty). Connection
    /// failures are silently absorbed — call `connected()` to check.
    explicit DsConfig(std::string socketPath = {});
    ~DsConfig();

    DsConfig(const DsConfig&)            = delete;
    DsConfig& operator=(const DsConfig&) = delete;

    bool connected() const { return m_ok; }
    const std::string& socket_path() const { return m_path; }

    std::optional<std::string>   endpoint();
    std::optional<std::string>   server_uri();
    std::optional<std::uint32_t> lifetime();

    /// Default socket path when none is supplied. Tracks ds-server's
    /// kDefaultSocketPath, duplicated here so the public header stays
    /// free of data_store/ includes.
    static const char* kDefaultSocketPath;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string           m_path;
    bool                  m_ok = false;
};

} // namespace iot

#endif /* __apps_ds_config_hpp__ */
