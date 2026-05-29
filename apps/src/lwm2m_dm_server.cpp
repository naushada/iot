#include "lwm2m_dm_server.hpp"

#include <sstream>

#include "lwm2m_coap_builder.hpp"
#include "lwm2m_codec_registry.hpp"   // ContentFormat enum: CF_LinkFormat, CF_LwM2MTlv

namespace lwm2m { namespace dmsrv {

using namespace ::lwm2m::coap;

std::string build_uri(std::uint32_t oid,
                      std::int32_t iid, std::int32_t rid, std::int32_t riid) {
    std::string out = "/" + std::to_string(oid);
    if (iid  >= 0) out += "/" + std::to_string(iid);
    if (rid  >= 0) out += "/" + std::to_string(rid);
    if (riid >= 0) out += "/" + std::to_string(riid);
    return out;
}

namespace {

void emit_dm_path(std::ostringstream& ss, std::uint16_t& prev,
                  std::uint32_t oid, std::int32_t iid, std::int32_t rid) {
    emit_option(ss, OPT_URI_PATH, std::to_string(oid), prev);
    if (iid >= 0) emit_option(ss, OPT_URI_PATH, std::to_string(iid), prev);
    if (rid >= 0) emit_option(ss, OPT_URI_PATH, std::to_string(rid), prev);
}

} // namespace

std::string build_read(std::uint16_t messageId,
                       const std::string& token,
                       std::uint32_t oid,
                       std::int32_t iid, std::int32_t rid,
                       std::int32_t acceptCf) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_GET, TYPE_CON);
    std::uint16_t prev = 0;
    emit_dm_path(ss, prev, oid, iid, rid);
    if (acceptCf >= 0) {
        emit_option(ss, OPT_ACCEPT,
                    cf_bytes(static_cast<std::uint16_t>(acceptCf)), prev);
    }
    return ss.str();
}

std::string build_discover(std::uint16_t messageId,
                           const std::string& token,
                           std::uint32_t oid,
                           std::int32_t iid, std::int32_t rid) {
    return build_read(messageId, token, oid, iid, rid, CF_LinkFormat);
}

std::string build_write(std::uint16_t messageId,
                        const std::string& token,
                        std::uint32_t oid,
                        std::int32_t iid, std::int32_t rid,
                        std::uint16_t cf,
                        const std::string& payload,
                        bool partial) {
    std::ostringstream ss;
    emit_header(ss, messageId, token,
                partial ? METHOD_POST : METHOD_PUT, TYPE_CON);
    std::uint16_t prev = 0;
    emit_dm_path(ss, prev, oid, iid, rid);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(cf), prev);
    if (!payload.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << payload;
    }
    return ss.str();
}

std::string build_create(std::uint16_t messageId,
                         const std::string& token,
                         std::uint32_t oid,
                         const std::string& tlvPayload) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_POST, TYPE_CON);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_URI_PATH, std::to_string(oid), prev);
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(CF_LwM2MTlv), prev);
    if (!tlvPayload.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << tlvPayload;
    }
    return ss.str();
}

std::string build_delete(std::uint16_t messageId,
                         const std::string& token,
                         std::uint32_t oid, std::int32_t iid) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_DELETE, TYPE_CON);
    std::uint16_t prev = 0;
    emit_dm_path(ss, prev, oid, iid, /*rid*/ -1);
    return ss.str();
}

std::string build_execute(std::uint16_t messageId,
                          const std::string& token,
                          std::uint32_t oid,
                          std::int32_t iid, std::int32_t rid,
                          const std::string& args) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_POST, TYPE_CON);
    std::uint16_t prev = 0;
    emit_dm_path(ss, prev, oid, iid, rid);
    if (!args.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << args;
    }
    return ss.str();
}

std::string build_write_attributes(std::uint16_t messageId,
                                   const std::string& token,
                                   std::uint32_t oid,
                                   std::int32_t iid, std::int32_t rid,
                                   const AttributeUpdate& upd) {
    std::ostringstream ss;
    emit_header(ss, messageId, token, METHOD_PUT, TYPE_CON);
    std::uint16_t prev = 0;
    emit_dm_path(ss, prev, oid, iid, rid);
    auto fmt = [](double d) {
        std::ostringstream s; s << d; return s.str();
    };
    if (upd.pmin) emit_option(ss, OPT_URI_QUERY, "pmin=" + std::to_string(*upd.pmin), prev);
    if (upd.pmax) emit_option(ss, OPT_URI_QUERY, "pmax=" + std::to_string(*upd.pmax), prev);
    if (upd.gt)   emit_option(ss, OPT_URI_QUERY, "gt="   + fmt(*upd.gt),   prev);
    if (upd.lt)   emit_option(ss, OPT_URI_QUERY, "lt="   + fmt(*upd.lt),   prev);
    if (upd.st)   emit_option(ss, OPT_URI_QUERY, "st="   + fmt(*upd.st),   prev);
    return ss.str();
}

}} // namespace lwm2m::dmsrv
