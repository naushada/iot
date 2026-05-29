#ifndef __lwm2m_codec_plaintext_hpp__
#define __lwm2m_codec_plaintext_hpp__

#include <cstdint>
#include <string>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_codec_plaintext.hpp
 * @brief Plain-text (CF=0) single-resource encoder + decoder.
 *
 * Closes REQ-ENC-005 per Core §6.4. Used for GET/PUT against a single
 * Resource URI of type String, Integer, Float, Boolean, or Time. Anything
 * else (Opaque, ObjLink, multi-instance) must use opaque (CF=42) or TLV
 * (CF=11542) — those go through different codec modules.
 *
 * Spec rules:
 *   - Integer: signed decimal, optional leading "-", no leading zeros.
 *   - Float:   decimal with `.` and optional exponent.
 *   - Boolean: literal "0" or "1".
 *   - String:  raw UTF-8 bytes, no quoting.
 *   - Time:    Integer (seconds since the Unix epoch).
 */

namespace lwm2m { namespace plaintext {

/// Encode a single resource value to wire bytes per its `ResourceType`.
/// Returns 0 on success, -1 on an unsupported type.
int encode(ResourceType type, const std::string& value, std::string& out);

/// Decode wire bytes into the raw string the Resource::write callback
/// expects. Boolean must be "0" or "1"; numerics must parse cleanly.
/// Returns 0 on success, -1 on a malformed numeric / boolean.
int decode(ResourceType type, const std::string& bytes, std::string& out);

}} // namespace lwm2m::plaintext

#endif /*__lwm2m_codec_plaintext_hpp__*/
