#ifndef __data_store_dep_watch_hpp__
#define __data_store_dep_watch_hpp__

/// Dependency-state watch, shared across iot daemons (L17a/D1).
///
/// Composes with ServiceGate: ServiceGate owns the enable/disable
/// fence; DepWatch owns the "are my dependencies healthy?" fence.
/// The Supervisor evaluates both in its wake loop — if ANY dep is
/// disabled, gate.reason="dep_down:<name>" regardless of the
/// ServiceGate's own enabled() value.
///
/// Composition priority: dep_down > disabled > wan_down/conflict.
///
/// Threading
/// ---------
/// Same pattern as ServiceGate. The data_store::Client's internal
/// listener thread updates snapshots under a mutex; the Supervisor's
/// main thread calls healthy() and wait(). `wait()` blocks until a
/// dependency state changes OR `shutdown()` is called.
///
/// One DepWatch per daemon with dependencies. Daemons with no
/// dependencies (net-router) don't construct one.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace data_store {

class Client;   // forward

class DepWatch {
public:
    /// `client` MUST be a connected data_store::Client (the caller
    /// keeps ownership). `deps` is the list of bare daemon names
    /// this daemon depends on (e.g. {"net.router"}). For each dep,
    /// DepWatch subscribes to `services.<dep>.state` via the
    /// Client's watch mechanism and primes its snapshot via get().
    DepWatch(Client& client, std::vector<std::string> deps);
    ~DepWatch();

    DepWatch(const DepWatch&)            = delete;
    DepWatch& operator=(const DepWatch&) = delete;

    /// True when every dependency is in a healthy state. "Healthy"
    /// means the state key is absent/unset (schema default "running"),
    /// or its value is "running" or "starting". Any other value
    /// ("disabled", "stopping", "exited", "conflict") is unhealthy.
    bool healthy() const;

    /// Returns the bare name of the first unhealthy dependency
    /// (e.g. "net.router"), or empty string when healthy.
    std::string unhealthy_dep() const;

    /// Block until a dependency state changes OR `shutdown()` is
    /// called. Returns true on a state change, false on shutdown.
    /// Safe to call from multiple threads; every waiter wakes on
    /// any dep state transition.
    bool wait();

    /// Wake any thread blocked in `wait()`. Idempotent.
    void shutdown();

    /// Number of declared dependencies.
    std::size_t count() const { return m_deps.size(); }

    /// Bare dependency names passed to the ctor.
    const std::vector<std::string>& deps() const { return m_deps; }

private:
    Client&                  m_client;
    std::vector<std::string> m_deps;          // bare names: "net.router"
    std::vector<std::string> m_state_keys;    // "services.net.router.state"

    mutable std::mutex       m_mtx;
    std::condition_variable  m_cv;
    std::vector<bool>        m_healthy;       // one per dep
    bool                     m_shutdown = false;
    std::uint64_t            m_version  = 0;

    std::vector<std::uint64_t> m_watch_handles;  // one per dep watch

    static std::string make_state_key(const std::string& name);
    static bool is_healthy_state(std::string_view s);
};

} // namespace data_store

#endif /* __data_store_dep_watch_hpp__ */
