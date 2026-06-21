#ifndef __lwm2m_sample_buffer_hpp__
#define __lwm2m_sample_buffer_hpp__

#include <cstddef>
#include <deque>
#include <vector>

#include "lwm2m_telemetry_pack.hpp"

/**
 * @file lwm2m_sample_buffer.hpp
 * @brief Bounded FIFO of telemetry Samples awaiting LwM2M Send, with
 *        ACK-then-prune (§3b) delivery semantics.
 *
 * The client uploader accumulates timestamped readings here while the cloud
 * link is available; it `take()`s an oldest-first batch to Send, and either
 * drops it on a 2.04 ACK or `requeue()`s it on failure so it retries first.
 * Bounded: when full, the oldest queued sample is evicted (recent telemetry
 * wins; the deep history is the cloud spool's job, PR-22/23). Pure — no ACE /
 * CoAP — so it is fully unit-testable; the transmit/timer wiring lives in the
 * registered client session.
 */

namespace lwm2m { namespace telemetry {

class SampleBuffer {
public:
    /// `capacity` samples max (clamped to >= 1).
    explicit SampleBuffer(std::size_t capacity)
        : m_cap(capacity ? capacity : 1) {}

    /// Append a reading; evicts the oldest when full (counts it in dropped()).
    void push(const Sample& s) {
        if (m_q.size() >= m_cap) { m_q.pop_front(); ++m_dropped; }
        m_q.push_back(s);
    }

    std::size_t size() const { return m_q.size(); }
    bool        empty() const { return m_q.empty(); }
    /// Total samples evicted by overflow (here + requeue), for telemetry.
    std::size_t dropped() const { return m_dropped; }

    /// Remove and return up to `n` oldest samples — the batch to Send.
    std::vector<Sample> take(std::size_t n) {
        std::vector<Sample> out;
        const std::size_t k = n < m_q.size() ? n : m_q.size();
        out.reserve(k);
        for (std::size_t i = 0; i < k; ++i) {
            out.push_back(std::move(m_q.front()));
            m_q.pop_front();
        }
        return out;
    }

    /// Return a taken batch to the FRONT after a failed Send (nack/timeout) so
    /// it retries first, preserving order. Honours capacity: if the buffer
    /// filled while the batch was in flight, the oldest overflow is dropped
    /// (newest-wins), counted in dropped().
    void requeue(std::vector<Sample>&& batch) {
        for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
            if (m_q.size() >= m_cap) { ++m_dropped; continue; }
            m_q.push_front(std::move(*it));
        }
        batch.clear();
    }

private:
    std::deque<Sample> m_q;
    std::size_t        m_cap;
    std::size_t        m_dropped{0};
};

}} // namespace lwm2m::telemetry

#endif /*__lwm2m_sample_buffer_hpp__*/
