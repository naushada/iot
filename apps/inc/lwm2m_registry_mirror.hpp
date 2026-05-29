#ifndef __lwm2m_registry_mirror_hpp__
#define __lwm2m_registry_mirror_hpp__

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <ace/Task.h>
#include <ace/Synch_Traits.h>

#include "lwm2m_registration.hpp"

class DbClient; // forward — defined in db_adapter.hpp; the mirror's only
                // dependency on Mongo is through this opaque pointer so
                // tests can substitute a stub.

/**
 * @file lwm2m_registry_mirror.hpp
 * @brief Async MongoDB mirror for the server-side ClientRegistry.
 *
 * D3 (RDD §11 row D3): the ClientRegistry is authoritative in memory; this
 * worker drains a bounded queue of RegistryEvent records onto MongoDB so
 * registrations survive a server restart. The reactor thread enqueues and
 * returns — it never blocks on DB I/O. On worker death the in-memory copy
 * remains intact and the next restart of the worker re-publishes a full
 * snapshot.
 *
 * Scope discipline: the actual BSON document shape and the `DbClient`
 * method calls are stubbed in the .cpp with detailed TODOs. The L3 PR
 * delivers the queue + ACE_Task + RegistryEvent shape so the
 * RegistrationServer can fire events without knowing how they'll be
 * persisted. The Mongo schema is its own (small) follow-up — see
 * lwm2m-design.md §12 D3 / RDD REQ-REG-010.
 */

namespace lwm2m {

/// One mutation event that needs to be mirrored.
struct RegistryEvent {
    enum Op : std::uint8_t { Add, Update, Remove };

    Op                  op{Add};
    std::string         location;       ///< populated for all ops
    ServerRegistration  snapshot;       ///< populated for Add / Update (snapshot at event time)
};

/**
 * @brief Worker thread that drains a bounded queue of RegistryEvents.
 *
 * Lifecycle:
 *   - Construct with a DbClient reference (the existing process-wide
 *     instance).
 *   - Call `open()` once from the main thread; spawns one worker.
 *   - `post()` from the reactor (or any thread) — non-blocking, returns
 *     -1 if the queue is at the high-water mark.
 *   - `stop()` requests shutdown; `wait()` joins.
 *
 * The mirror does NOT subscribe to the ClientRegistry directly — wiring is
 * done by whoever owns both objects (RegistrationServer at construction).
 * That keeps the registry free of Mongo coupling and makes test setup
 * trivial.
 */
class RegistryMirror : public ACE_Task<ACE_MT_SYNCH> {
public:
    /// Bounded-queue high-water mark. Above this, posts are dropped
    /// (mirror only — the in-memory authoritative copy is never affected)
    /// and `m_dropped` is incremented. See RISK-06.
    static constexpr std::size_t kHighWater = 4096;

    explicit RegistryMirror(DbClient* db);
    ~RegistryMirror() override;

    /// ACE_Task overrides.
    int open(void* args = nullptr) override;
    int svc() override;

    /// Enqueue an event. Returns 0 on success, -1 on overflow (the event
    /// is dropped; the in-memory registry remains the source of truth).
    int post(RegistryEvent ev);

    /// Request shutdown and join the worker.
    void stop();

    /// Synchronously rebuild the in-memory registry from MongoDB. Called
    /// once at server startup before the reactor's listening socket is
    /// registered. Returns the persisted registrations (empty on
    /// reachability failure — caller starts empty + logs a warning per
    /// design §12 D3).
    std::vector<ServerRegistration> reconstruct();

    /// Telemetry. Lock-free reads; values are best-effort.
    std::uint64_t enqueued() const { return m_enqueued.load(); }
    std::uint64_t flushed()  const { return m_flushed.load(); }
    std::uint64_t dropped()  const { return m_dropped.load(); }

private:
    DbClient*                     m_db;        ///< not owned; nullable in tests
    std::atomic<bool>             m_stop{false};
    std::mutex                    m_mu;
    std::queue<RegistryEvent>     m_queue;

    std::atomic<std::uint64_t>    m_enqueued{0};
    std::atomic<std::uint64_t>    m_flushed{0};
    std::atomic<std::uint64_t>    m_dropped{0};

    /// Internal: persist one event to Mongo. Stubbed for L3; see .cpp.
    void persist(const RegistryEvent& ev);
};

} // namespace lwm2m

#endif /*__lwm2m_registry_mirror_hpp__*/
