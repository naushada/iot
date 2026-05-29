#ifndef __lwm2m_coap_builder_hpp__
#define __lwm2m_coap_builder_hpp__

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <sstream>
#include <string>

#include "coap_adapter.hpp"

/**
 * @file lwm2m_coap_builder.hpp
 * @brief Header-only CoAP frame emitter.
 *
 * Shared by the L3 RegistrationClient, the L4 BootstrapClient /
 * BootstrapServer, and the L5 DM client/server. Replaces three nearly
 * identical copies of `emit_header` / `emit_option` / `extend_field` from
 * those translation units (RDD L4 cleanup follow-up).
 *
 * Kept header-only so it can be inlined cheaply per call-site and the test
 * build doesn't need yet another .cpp.
 */

namespace lwm2m { namespace coap {

constexpr std::uint16_t OPT_URI_HOST         = 3;
constexpr std::uint16_t OPT_LOCATION_PATH    = 8;
constexpr std::uint16_t OPT_URI_PATH         = 11;
constexpr std::uint16_t OPT_CONTENT_FORMAT   = 12;
constexpr std::uint16_t OPT_URI_QUERY        = 15;
constexpr std::uint16_t OPT_ACCEPT           = 17;
constexpr std::uint16_t OPT_LOCATION_QUERY   = 20;

constexpr std::uint8_t TYPE_CON = 0;
constexpr std::uint8_t TYPE_NON = 1;
constexpr std::uint8_t TYPE_ACK = 2;
constexpr std::uint8_t TYPE_RST = 3;

constexpr std::uint8_t METHOD_GET    = 1;
constexpr std::uint8_t METHOD_POST   = 2;
constexpr std::uint8_t METHOD_PUT    = 3;
constexpr std::uint8_t METHOD_DELETE = 4;

constexpr std::uint8_t RSP_201_CREATED  = 0x41;
constexpr std::uint8_t RSP_202_DELETED  = 0x42;
constexpr std::uint8_t RSP_204_CHANGED  = 0x44;
constexpr std::uint8_t RSP_205_CONTENT  = 0x45;
constexpr std::uint8_t RSP_400_BAD_REQ  = 0x80;
constexpr std::uint8_t RSP_401_UNAUTH   = 0x81;
constexpr std::uint8_t RSP_404_NOT_FND  = 0x84;
constexpr std::uint8_t RSP_405_METHOD   = 0x85;
constexpr std::uint8_t RSP_406_NOT_ACC  = 0x86;
constexpr std::uint8_t RSP_415_UNSUP_CF = 0x8F;
constexpr std::uint8_t RSP_500_INTERNAL = 0xA0;

/// Encode a 12-bit value into the (nibble, extension-bytes) pair used by
/// CoAP option header delta/length fields.
inline void extend_field(std::uint16_t v, std::uint8_t& nib, std::string& ext) {
    if (v < 13) { nib = static_cast<std::uint8_t>(v); return; }
    if (v < 269) {
        nib = 13;
        ext.push_back(static_cast<char>(v - 13));
        return;
    }
    nib = 14;
    std::uint16_t w = htons(static_cast<std::uint16_t>(v - 269));
    ext.append(reinterpret_cast<char*>(&w), 2);
}

/// Append one CoAP option to `ss`. `prevNumber` is the running cumulative
/// option number (start at 0 for the first option in a message).
inline void emit_option(std::ostringstream& ss,
                        std::uint16_t number,
                        const std::string& value,
                        std::uint16_t& prevNumber) {
    std::uint8_t dnib, lnib;
    std::string  dext, lext;
    extend_field(number - prevNumber, dnib, dext);
    extend_field(static_cast<std::uint16_t>(value.size()), lnib, lext);
    prevNumber = number;
    std::uint8_t hdr = (dnib << 4) | (lnib & 0x0F);
    ss.put(static_cast<char>(hdr));
    ss << dext << lext << value;
}

/// Append the 4-byte CoAP header + 0..8-byte token. `type` is one of the
/// TYPE_* constants; `code` is the method (request) or response code.
inline void emit_header(std::ostringstream& ss,
                        std::uint16_t messageId,
                        const std::string& token,
                        std::uint8_t code,
                        std::uint8_t type) {
    std::uint8_t tkl = static_cast<std::uint8_t>(std::min<std::size_t>(8, token.size()));
    std::uint8_t b0  = (1u << 6) | ((type & 0x03) << 4) | (tkl & 0x0F);
    std::uint16_t midBe = htons(messageId);
    ss.put(static_cast<char>(b0));
    ss.put(static_cast<char>(code));
    ss.write(reinterpret_cast<char*>(&midBe), 2);
    ss.write(token.data(), tkl);
}

/// Split "/a/b/c" into successive Uri-Path options. Skips empty
/// segments so a stray double slash doesn't break the round trip.
inline void emit_uri_path(std::ostringstream& ss,
                          const std::string& path,
                          std::uint16_t& prev) {
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] != '/') { ++i; continue; }
        ++i;
        std::size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) {
            emit_option(ss, OPT_URI_PATH, path.substr(start, i - start), prev);
        }
    }
}

/// Build a 2-byte big-endian content-format option value.
inline std::string cf_bytes(std::uint16_t cf) {
    std::string out;
    out.push_back(static_cast<char>((cf >> 8) & 0xFF));
    out.push_back(static_cast<char>(cf & 0xFF));
    return out;
}

/// Stamp a response onto an existing request message — preserves message
/// ID and token, sets type=ACK and the response code.
inline std::string build_ack(const CoAPAdapter::CoAPMessage& msg,
                             std::uint8_t code) {
    std::ostringstream ss;
    emit_header(ss, msg.coapheader.msgid,
                std::string(msg.tokens.begin(), msg.tokens.end()),
                code, TYPE_ACK);
    return ss.str();
}

/// Concatenate consecutive Uri-Path option values into "/a/b/c" form.
inline std::string join_uri(const CoAPAdapter::CoAPMessage& msg,
                            CoAPAdapter& coap) {
    std::string out;
    for (const auto& opt : msg.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) == "Uri-Path") {
            out.push_back('/');
            out.append(opt.optionvalue);
        }
    }
    return out;
}

/// Get a single Uri-Query value matching `key=`. Empty if absent.
inline std::string get_query(const CoAPAdapter::CoAPMessage& msg,
                             CoAPAdapter& coap,
                             const std::string& key) {
    const std::string prefix = key + "=";
    for (const auto& opt : msg.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) != "Uri-Query") continue;
        if (opt.optionvalue.size() > prefix.size() &&
            opt.optionvalue.compare(0, prefix.size(), prefix) == 0) {
            return opt.optionvalue.substr(prefix.size());
        }
    }
    return {};
}

/// Detect a flag-style Uri-Query option (no `=`), e.g. `obs`.
inline bool has_query_flag(const CoAPAdapter::CoAPMessage& msg,
                           CoAPAdapter& coap,
                           const std::string& key) {
    for (const auto& opt : msg.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) != "Uri-Query") continue;
        if (opt.optionvalue == key) return true;
    }
    return false;
}

}} // namespace lwm2m::coap

#endif /*__lwm2m_coap_builder_hpp__*/
