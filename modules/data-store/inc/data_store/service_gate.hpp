#ifndef __data_store_service_gate_hpp__
#define __data_store_service_gate_hpp__

/// Service-enable gate, shared across all iot daemons (L16/D1).
///
/// Wraps the (DsBridge -> on_change -> Supervisor) pattern that
/// openvpn-client's WAN gate already uses, but for the
/// services.<name>.enable key + the matching services.<name>.state
/// publish.
///
/// One ServiceGate per daemon. Composes with other gates (WAN gate
/// in openvpn-client, NM-conflict gate in wifi-client) — the
/// composition rule from log/L16/plan.md is: `enabled()==false`
/// dominates every other gate (the daemon's Supervisor evaluates
/// `if (!gate.enabled()) park`; other gates only get consulted
/// when enable is true).
///
/// Threading
/// ---------
/// - The data_store::Client's internal listener thread is the sole
///   writer of the snapshot value.
/// - `enabled()` is lock-protected; safe from any thread.
/// - `wait()` blocks the calling thread on a condition variable;
///   the listener wakes it on a value change OR `shutdown()`.
/// - `publish_state()` is a thin set() that logs failures via
///   ACE_ERROR and never throws — same posture as DsBridge's
///   setters in modules/openvpn/client/src/ds_bridge.cpp.
///
/// The header keeps data_store::Client out of the include surface
/// by forward-declaring it; the cpp pulls in the full type.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace data_store {

class Client;   // forward — defined in data_store/client.hpp

class ServiceGate {
public:
    /// `client` MUST be a connected data_store::Client (the caller
    /// keeps ownership). `name` is the bare daemon identifier
    /// (e.g. "openvpn.client"); the gate constructs the full key
    /// names from it as:
    ///   services.<name>.enable    (read + watch)
    ///   services.<name>.state     (write)
    ServiceGate(Client& client, std::string name);
    ~ServiceGate();

    ServiceGate(const ServiceGate&)            = delete;
    ServiceGate& operator=(const ServiceGate&) = delete;

    /// Last observed value. True when the key is absent or unset
    /// (the schema default is `true`). Lock-protected.
    bool enabled() const;

    /// Block until the snapshot value changes OR `shutdown()` is
    /// called. Returns the new value, or `nullopt` on shutdown.
    /// Multiple threads may wait concurrently; every waiter wakes
    /// on a change event (NFR-SVC-005).
    std::optional<bool> wait();

    /// Wake any thread blocked in `wait()`. Subsequent `enabled()`
    /// reflects the latest snapshot. Idempotent.
    void shutdown();

    /// Publish a new value for `services.<name>.state`. Best-effort:
    /// wire-level failures log via ACE_ERROR and return without
    /// throwing. Documented enum values: running / disabled /
    /// starting / stopping / exited / conflict.
    void publish_state(std::string_view s);

    /// The bare daemon name passed to the ctor (e.g. "openvpn.client").
    const std::string& name() const { return m_name; }

    /// The full schema key the gate watches.
    const std::string& enable_key() const { return m_enable_key; }
    const std::string& state_key()  const { return m_state_key;  }

private:
    Client&                  m_client;
    std::string              m_name;
    std::string              m_enable_key;
    std::string              m_state_key;

    mutable std::mutex       m_mtx;
    std::condition_variable  m_cv;
    bool                     m_enabled  = true;   // schema default
    std::uint64_t            m_version  = 0;      // bumped on every observed change
    bool                     m_shutdown = false;

    std::uint64_t            m_watch_handle = 0;  // 0 = not registered
};

} // namespace data_store

#endif /* __data_store_service_gate_hpp__ */
