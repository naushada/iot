#include "lwm2m_send_server.hpp"

#include "lwm2m_coap_builder.hpp"
#include "lwm2m_codec_senml.hpp"
#include "lwm2m_send.hpp"

namespace lwm2m {

using namespace ::lwm2m::coap;

SendOutcome SendServer::handle(const CoAPAdapter::CoAPMessage& msg,
                               CoAPAdapter& coap) {
    SendOutcome out;

    // Must be POST. (code low 5 bits = method class 0, detail = method.)
    if ((msg.coapheader.code & 0x1F) != METHOD_POST) return out;   // None

    // Uri-Path must be exactly "dp".
    std::string path;
    int cf = -1;
    for (const auto& opt : msg.uripath) {
        const std::string num = coap.getOptionNumber(opt.optiondelta);
        if (num == "Uri-Path") {
            if (!path.empty()) path += "/";
            path += opt.optionvalue;
        } else if (num == "Content-Format" && !opt.optionvalue.empty()) {
            cf = 0;
            for (unsigned char b : opt.optionvalue) cf = (cf << 8) | b;  // CoAP uint
        }
    }
    if (path != "dp") return out;   // None — not a Send

    // Decode the SenML pack per its Content-Format.
    std::vector<senml::Record> recs;
    int rc;
    if (cf == send::CF_SENML_CBOR) {
        rc = senml::decode_cbor(msg.payload, recs);
    } else if (cf == send::CF_SENML_JSON) {
        rc = senml::decode_json(msg.payload, recs);
    } else {
        out.kind     = SendOutcome::UnsupportedFormat;
        out.response = build_ack(msg, RSP_415_UNSUP_CF);
        return out;
    }
    if (rc != 0) {
        out.kind     = SendOutcome::BadRequest;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }

    out.basePath = recs.empty() ? std::string() : recs.front().baseName;
    out.samples  = telemetry::parse_pack(recs);
    out.kind     = SendOutcome::Reported;
    out.response = build_ack(msg, RSP_204_CHANGED);
    return out;
}

} // namespace lwm2m
