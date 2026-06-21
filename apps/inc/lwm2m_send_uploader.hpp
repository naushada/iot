#ifndef __lwm2m_send_uploader_hpp__
#define __lwm2m_send_uploader_hpp__

#include <cstdint>
#include <string>
#include <vector>

#include "lwm2m_sample_buffer.hpp"
#include "lwm2m_send.hpp"

/**
 * @file lwm2m_send_uploader.hpp
 * @brief Stop-and-wait LwM2M Send uploader (client side).
 *
 * Ties together the buffer (PR-33), the pack builder (PR-28), the SenML codec
 * (PR-25), and the /dp frame builder (PR-29) into the client upload loop:
 * accumulate samples, emit ONE CON /dp frame at a time, prune the batch on its
 * 2.04 ACK, requeue it on timeout. Stop-and-wait keeps it simple + correct for
 * confirmable Send and bounds memory to a single in-flight batch.
 *
 * Pure — no ACE/UDP/timers. The registered DM client session owns the actual
 * transmit, the msg-id/token allocation, and feeds back on_ack/on_timeout. That
 * keeps the upload policy fully unit-testable; only the I/O glue is HW-verified.
 */

namespace lwm2m { namespace send {

class Uploader {
public:
    /// `basePath` = SenML bn (e.g. "/33000/0/"); `capacity` = buffer depth;
    /// `maxBatch` = max samples per Send (keep small so packs stay < 1 block
    /// until Block-Wise lands).
    Uploader(std::string basePath, std::size_t capacity, std::size_t maxBatch,
             int contentFormat = CF_SENML_CBOR)
        : m_buf(capacity), m_basePath(std::move(basePath)),
          m_maxBatch(maxBatch ? maxBatch : 1), m_cf(contentFormat) {}

    /// Producer: enqueue a reading.
    void offer(const telemetry::Sample& s) { m_buf.push(s); }

    /// Queued samples not counting the in-flight batch.
    std::size_t pending() const { return m_buf.size(); }
    bool in_flight() const { return m_inFlight; }
    std::uint16_t in_flight_msgid() const { return m_inflightMsgId; }
    std::size_t dropped() const { return m_buf.dropped(); }

    /// Emit the next /dp Send frame when nothing is in flight and samples are
    /// pending; marks the taken batch in flight under (msgId, token). Returns
    /// the CoAP wire bytes, or empty when there's nothing to send / a batch is
    /// already awaiting ACK. The caller transmits the bytes over the session.
    std::string poll(std::uint16_t msgId, const std::string& token);

    /// Server ACK'd msgId → prune the in-flight batch (delivered).
    void on_ack(std::uint16_t msgId);
    /// Retransmit window elapsed with no ACK → requeue the in-flight batch so
    /// the next poll() re-sends it (oldest-first preserved).
    void on_timeout();

private:
    telemetry::SampleBuffer        m_buf;
    std::string                    m_basePath;
    std::size_t                    m_maxBatch;
    int                            m_cf;
    std::vector<telemetry::Sample> m_inflight;
    bool                           m_inFlight{false};
    std::uint16_t                  m_inflightMsgId{0};
};

}} // namespace lwm2m::send

#endif /*__lwm2m_send_uploader_hpp__*/
