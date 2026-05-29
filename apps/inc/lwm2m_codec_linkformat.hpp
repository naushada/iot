#ifndef __lwm2m_codec_linkformat_hpp__
#define __lwm2m_codec_linkformat_hpp__

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_codec_linkformat.hpp
 * @brief RFC 6690 "CoRE Link Format" encoder + parser for LwM2M.
 *
 * L2 deliverable per apps/docs/lwm2m-design.md §4.4 / apps/docs/lwm2m-rdd.md
 * REQ-ENC-007, REQ-REG-008, REQ-DM-002.
 *
 * link-format is not a LwM2MObject codec (its payload describes a link list,
 * not a TLV record set), so it deliberately does NOT implement ICodec from
 * lwm2m_codec_registry. The two interfaces live side by side. CoAPAdapter
 * routes content-format 40 here directly.
 */

namespace lwm2m { namespace linkformat {

/// One entry in a link-format payload: a URI plus zero or more name=value
/// attributes. Per RFC 6690 §2 the value may be a bare token, a quoted
/// string, or absent (flag-style). We store it as a plain string and
/// remember whether the original was quoted so we round-trip cleanly.
struct LinkAttr {
    std::string name;
    std::string value;       ///< empty when the attribute is flag-style ("obs")
    bool        quoted{false};
};

struct LinkEntry {
    std::string             uri;             ///< e.g. "/1/0", "/", "/3/0/13"
    std::vector<LinkAttr>   attrs;

    /// Append `name=value` with quoting. `force_quote` is for string-typed
    /// values that must round-trip with quotes even if they have no spaces
    /// (`rt="oma.lwm2m"`, `ver="1.1"`).
    LinkEntry& set(std::string name, std::string value, bool force_quote = false);
    LinkEntry& set(std::string name, std::uint64_t value);
    /// Flag-style: just `;obs` with no value.
    LinkEntry& set_flag(std::string name);

    /// Lookup helper for tests / parsers. Returns nullptr if absent.
    const LinkAttr* find(const std::string& name) const;
};

/* ────────── Encoding / decoding ────────── */

/// Encode a list of entries to an RFC 6690 byte string.
/// Format: `<uri>(;name[=value])*(,<uri>...)*`
std::string encode(const std::vector<LinkEntry>& entries);

/// Decode an RFC 6690 byte string into entries.
/// Returns 0 on success, -1 on malformed input (unbalanced quotes,
/// missing `<`/`>`, empty URI). Partial output is left in `out`.
int decode(const std::string& text, std::vector<LinkEntry>& out);

/* ────────── LwM2M helpers ────────── */

/**
 * @brief Build the Register payload link-format for an ObjectStore.
 *
 * Output shape (Core §6.2.2):
 *   `</>;rt="oma.lwm2m";ct=11542,</1/0>;ver="1.1",</3/0>,</5/0>`
 *
 * - The root `</>` carries `rt="oma.lwm2m"` and `ct={list of supported
 *   content-format codes}` (REQ-REG-008).
 * - For each Object with one or more instances, emit one entry per
 *   instance. Object version (`ver="1.1"`) is emitted on the first
 *   instance whose descriptor declares a non-default version.
 *
 * Decision D2 (single-server v1, keyed by Short Server ID): each call
 * emits one payload; observers on Server Object 1/N are dispatched
 * separately and do not affect the link-format root.
 */
std::vector<LinkEntry> register_payload(const ObjectStore& store);

/**
 * @brief Build the Discover response link-format for a URI.
 *
 * - `oid` always set.
 * - `iid < 0` ⇒ describe the object descriptor and all instances.
 * - `iid ≥ 0, rid < 0` ⇒ describe one instance and its resources.
 * - `iid ≥ 0, rid ≥ 0` ⇒ describe one resource (with per-resource
 *    notification attributes pulled from the Short Server ID matching
 *    `shortServerId`, per D2).
 *
 * Returns an empty vector if the addressed object/instance/resource
 * doesn't exist; the L5 handler turns that into 4.04 Not Found.
 */
std::vector<LinkEntry> discover(const ObjectStore& store,
                                std::uint32_t oid,
                                std::int32_t  iid,
                                std::int32_t  rid,
                                std::uint16_t shortServerId);

}} // namespace lwm2m::linkformat

#endif /*__lwm2m_codec_linkformat_hpp__*/
