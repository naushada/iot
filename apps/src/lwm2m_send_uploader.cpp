#include "lwm2m_send_uploader.hpp"

#include "lwm2m_codec_senml.hpp"
#include "lwm2m_telemetry_pack.hpp"

namespace lwm2m { namespace send {

std::string Uploader::poll(std::uint16_t msgId, const std::string& token) {
    if (m_inFlight) return {};            // stop-and-wait: one batch at a time
    if (m_buf->empty()) return {};

    m_inflight = m_buf->take(m_maxBatch);
    if (m_inflight.empty()) return {};

    auto recs = telemetry::build_pack(m_basePath, m_inflight);
    std::string body = (m_cf == CF_SENML_JSON) ? senml::encode_json(recs)
                                               : senml::encode_cbor(recs);
    m_inFlight      = true;
    m_inflightMsgId = msgId;
    return build_send_request(msgId, token, body, m_cf);
}

void Uploader::on_ack(std::uint16_t msgId) {
    if (m_inFlight && msgId == m_inflightMsgId) {
        m_buf->commit();                  // durable buffer frees the leased batch
        m_inflight.clear();
        m_inFlight = false;               // delivered → pruned
    }
}

void Uploader::on_timeout() {
    if (m_inFlight) {
        m_buf->requeue(std::move(m_inflight));  // retry first, oldest-first kept
        m_inflight.clear();
        m_inFlight = false;
    }
}

}} // namespace lwm2m::send
