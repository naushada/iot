#ifndef __lwm2m_codec_opaque_hpp__
#define __lwm2m_codec_opaque_hpp__

#include <string>

/**
 * @file lwm2m_codec_opaque.hpp
 * @brief Opaque (CF=42) single-resource binary codec.
 *
 * Closes REQ-ENC-006. Used for GET/PUT against a single Resource URI of
 * type Opaque. Both directions are no-ops at the wire level — the bytes
 * already are the value — so this module exists for symmetry with the
 * plain-text codec and to give the DM dispatcher a single shape to call.
 */

namespace lwm2m { namespace opaque {

inline int encode(const std::string& value, std::string& out) {
    out = value;
    return 0;
}

inline int decode(const std::string& bytes, std::string& out) {
    out = bytes;
    return 0;
}

}} // namespace lwm2m::opaque

#endif /*__lwm2m_codec_opaque_hpp__*/
