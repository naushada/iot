#ifndef __lwm2m_durable_sample_buffer_hpp__
#define __lwm2m_durable_sample_buffer_hpp__

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lwm2m_sample_buffer.hpp"

struct sqlite3;

/**
 * @file lwm2m_durable_sample_buffer.hpp
 * @brief SQLite-backed ISampleBuffer — the on-device store-and-forward buffer
 *        (tdd-vehicle-telemetry.md §3a.1), the lightweight alternative to PR-7's
 *        on-device mongod.
 *
 * Identical queue semantics to the in-RAM SampleBuffer (bounded FIFO, oldest-
 * first take, requeue, overflow eviction) but DURABLE across reboot/crash and
 * beyond RAM, so a device that is offline for hours can backfill the cloud with
 * true per-point timestamps once the LwM2M/DTLS link returns.
 *
 * Crash-safety = lease, not delete: take() marks rows in-flight (sent=1) and
 * keeps them; the Uploader calls commit() on the 2.04 ACK (DELETE the batch) or
 * requeue() on timeout (sent=0, retried oldest-first). On open any sent=1 row is
 * re-armed (sent=0), so a crash mid-flight re-sends — at-least-once; the cloud
 * dedups by (endpoint, seq). No #ifdef IOT_ENABLE_MONGO: this compiles in both
 * device and cloud builds; the cloud just never points iot.telemetry.db.path at
 * a file.
 */

namespace lwm2m { namespace telemetry {

class DurableSampleBuffer final : public ISampleBuffer {
public:
    /// Open (creating if needed) the SQLite outbox at `path`. `cap` bounds the
    /// PENDING (sent=0) rows — over it, the oldest are evicted newest-wins like
    /// the RAM buffer. `ttlSeconds` > 0 arms a safety-net reaper (call
    /// reap_expired() from a timer). Throws std::runtime_error on open/bootstrap
    /// failure so the factory can fall back to the in-RAM buffer.
    DurableSampleBuffer(const std::string& path, std::size_t cap,
                        std::int64_t ttlSeconds);
    ~DurableSampleBuffer() override;

    DurableSampleBuffer(const DurableSampleBuffer&)            = delete;
    DurableSampleBuffer& operator=(const DurableSampleBuffer&) = delete;

    void                push(const Sample& s) override;
    std::size_t         size() const override;               ///< COUNT WHERE sent=0
    bool                empty() const override;
    std::size_t         dropped() const override;
    std::vector<Sample> take(std::size_t n) override;        ///< lease oldest n
    void                requeue(std::vector<Sample>&& b) override;  ///< un-lease
    void                commit() override;                   ///< delete leased

    /// DELETE rows older than ttlSeconds relative to `now` (epoch seconds).
    /// Returns rows reaped. No-op when ttl <= 0. Drive from the client tick.
    std::size_t reap_expired(std::time_t now);

private:
    void exec(const char* sql);                  ///< throw on error
    std::int64_t count_pending() const;

    sqlite3*                  m_db{nullptr};
    std::size_t               m_cap;
    std::int64_t              m_ttl;             ///< seconds; 0 = no reaper
    std::vector<std::int64_t> m_leased;          ///< seqs of the in-flight batch
    std::size_t               m_dropped{0};      ///< overflow + ttl evictions
    mutable std::mutex        m_mtx;             ///< producer vs session thread
};

/// Pick a buffer at runtime: a non-empty `dbPath` (and a successful open) →
/// DurableSampleBuffer; empty path OR open failure → in-RAM SampleBuffer (so
/// telemetry still flows live, just without offline backfill). Never throws.
std::unique_ptr<ISampleBuffer>
make_sample_buffer(const std::string& dbPath, std::size_t cap,
                   std::int64_t ttlSeconds);

}} // namespace lwm2m::telemetry

#endif /*__lwm2m_durable_sample_buffer_hpp__*/
