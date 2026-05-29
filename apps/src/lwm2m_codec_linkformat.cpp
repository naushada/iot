#include "lwm2m_codec_linkformat.hpp"

#include <cctype>
#include <cstring>
#include <sstream>

namespace lwm2m { namespace linkformat {

/* ───────────────────────── LinkEntry helpers ──────────────────────────── */

LinkEntry& LinkEntry::set(std::string name, std::string value, bool force_quote) {
    LinkAttr a;
    a.name   = std::move(name);
    a.value  = std::move(value);
    a.quoted = force_quote;
    attrs.push_back(std::move(a));
    return *this;
}

LinkEntry& LinkEntry::set(std::string name, std::uint64_t value) {
    LinkAttr a;
    a.name   = std::move(name);
    a.value  = std::to_string(value);
    a.quoted = false;
    attrs.push_back(std::move(a));
    return *this;
}

LinkEntry& LinkEntry::set_flag(std::string name) {
    LinkAttr a;
    a.name   = std::move(name);
    a.value.clear();
    a.quoted = false;
    attrs.push_back(std::move(a));
    return *this;
}

const LinkAttr* LinkEntry::find(const std::string& name) const {
    for (const auto& a : attrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

/* ───────────────────────── encode ─────────────────────────────────────── */

namespace {

bool needs_quoting(const std::string& v) {
    if (v.empty()) return false;
    for (char c : v) {
        // RFC 6690 / RFC 5987: tokens may contain only "safe" characters.
        // Anything not in [A-Za-z0-9._-:] gets quoted. This is conservative
        // and matches what Leshan emits.
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '.' || c == '_' || c == '-' || c == ':')) {
            return true;
        }
    }
    return false;
}

std::string escape_quoted(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 2);
    for (char c : v) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void emit_attr(std::ostringstream& ss, const LinkAttr& a) {
    ss << ';' << a.name;
    if (a.value.empty() && !a.quoted) {
        // flag-style attribute: just ;name
        return;
    }
    ss << '=';
    if (a.quoted || needs_quoting(a.value)) {
        ss << '"' << escape_quoted(a.value) << '"';
    } else {
        ss << a.value;
    }
}

} // namespace

std::string encode(const std::vector<LinkEntry>& entries) {
    std::ostringstream ss;
    bool first = true;
    for (const auto& e : entries) {
        if (!first) ss << ',';
        first = false;
        ss << '<' << e.uri << '>';
        for (const auto& a : e.attrs) {
            emit_attr(ss, a);
        }
    }
    return ss.str();
}

/* ───────────────────────── decode ─────────────────────────────────────── */

namespace {

/// Skip over a quoted-string starting at text[i] (i points past the opening
/// quote). Returns the index just past the closing quote, or std::string::npos
/// on missing close.
std::size_t skip_quoted(const std::string& text, std::size_t i, std::string& out) {
    out.clear();
    while (i < text.size()) {
        char c = text[i++];
        if (c == '\\' && i < text.size()) {
            out.push_back(text[i++]);
        } else if (c == '"') {
            return i;
        } else {
            out.push_back(c);
        }
    }
    return std::string::npos;
}

/// Parse one entry starting at text[i]. On success returns the index past
/// the entry (pointing at either a ',' or end-of-text). On failure returns
/// std::string::npos.
std::size_t parse_entry(const std::string& text, std::size_t i, LinkEntry& out) {
    // Skip leading whitespace.
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;

    if (i >= text.size() || text[i] != '<') return std::string::npos;
    ++i;
    auto end_uri = text.find('>', i);
    if (end_uri == std::string::npos) return std::string::npos;
    out.uri.assign(text, i, end_uri - i);
    if (out.uri.empty()) return std::string::npos;
    i = end_uri + 1;

    // Parse zero or more ;name[=value] suffixes until we hit a comma or
    // run out of text.
    while (i < text.size() && text[i] != ',') {
        if (text[i] != ';') return std::string::npos;
        ++i;

        LinkAttr a;
        // Attribute name: chars up to '=', ';', or ','.
        while (i < text.size() && text[i] != '=' &&
               text[i] != ';' && text[i] != ',') {
            a.name.push_back(text[i++]);
        }
        if (a.name.empty()) return std::string::npos;

        if (i < text.size() && text[i] == '=') {
            ++i;
            if (i < text.size() && text[i] == '"') {
                ++i;
                std::size_t after = skip_quoted(text, i, a.value);
                if (after == std::string::npos) return std::string::npos;
                a.quoted = true;
                i = after;
            } else {
                while (i < text.size() && text[i] != ';' && text[i] != ',') {
                    a.value.push_back(text[i++]);
                }
                a.quoted = false;
            }
        } else {
            // flag-style: ;name with no value.
            a.quoted = false;
        }
        out.attrs.push_back(std::move(a));
    }
    return i;
}

} // namespace

int decode(const std::string& text, std::vector<LinkEntry>& out) {
    std::size_t i = 0;
    while (i < text.size()) {
        // Skip leading whitespace / stray commas at top level.
        while (i < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) {
            ++i;
        }
        if (i >= text.size()) break;

        LinkEntry e;
        auto next = parse_entry(text, i, e);
        if (next == std::string::npos) return -1;
        out.push_back(std::move(e));
        i = next;
    }
    return 0;
}

/* ───────────────────────── LwM2M helpers ──────────────────────────────── */

namespace {

/// Extract the "1.1" out of "urn:oma:lwm2m:oma:3:1.1" if present.
std::string version_from_urn(const std::string& urn) {
    auto pos = urn.rfind(':');
    if (pos == std::string::npos || pos + 1 >= urn.size()) return {};
    return urn.substr(pos + 1);
}

} // namespace

std::vector<LinkEntry> register_payload(const ObjectStore& store) {
    std::vector<LinkEntry> out;

    // Root entry per REQ-REG-008. ct enumerates the content-format codes
    // we can serve; for v1 that's just TLV (11542). L6 extends this with
    // SenML JSON / CBOR.
    LinkEntry root;
    root.uri = "/";
    root.set("rt", "oma.lwm2m", /*force_quote*/ true);
    root.set("ct", 11542u);
    out.push_back(std::move(root));

    for (const auto& [oid, desc] : store.objects()) {
        std::string version = version_from_urn(desc.urn);
        bool        version_emitted = false;

        if (desc.instances.empty()) {
            // Spec §6.2.2: an object with no instance is not advertised.
            continue;
        }

        for (const auto& [iid, _inst] : desc.instances) {
            LinkEntry e;
            e.uri = "/" + std::to_string(oid) + "/" + std::to_string(iid);
            if (!version.empty() && version != "1.0" && !version_emitted) {
                e.set("ver", version, /*force_quote*/ true);
                version_emitted = true;
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<LinkEntry> discover(const ObjectStore& store,
                                std::uint32_t oid,
                                std::int32_t  iid,
                                std::int32_t  rid,
                                std::uint16_t shortServerId) {
    std::vector<LinkEntry> out;

    const auto* desc = store.find(oid);
    if (!desc) return out;

    if (iid < 0) {
        // Whole object: emit the object root + one entry per instance, with
        // each instance's resources enumerated.
        LinkEntry root;
        root.uri = "/" + std::to_string(oid);
        std::string version = version_from_urn(desc->urn);
        if (!version.empty() && version != "1.0") {
            root.set("ver", version, /*force_quote*/ true);
        }
        out.push_back(std::move(root));

        for (const auto& [thisIid, inst] : desc->instances) {
            LinkEntry e;
            e.uri = "/" + std::to_string(oid) + "/" + std::to_string(thisIid);
            out.push_back(std::move(e));
            for (const auto& [thisRid, _res] : inst.resources) {
                LinkEntry r;
                r.uri = "/" + std::to_string(oid) + "/" + std::to_string(thisIid)
                      + "/" + std::to_string(thisRid);
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    const auto* inst = store.find(oid, static_cast<std::uint32_t>(iid));
    if (!inst) return out;

    if (rid < 0) {
        LinkEntry e;
        e.uri = "/" + std::to_string(oid) + "/" + std::to_string(iid);
        out.push_back(std::move(e));
        for (const auto& [thisRid, _res] : inst->resources) {
            LinkEntry r;
            r.uri = "/" + std::to_string(oid) + "/" + std::to_string(iid)
                  + "/" + std::to_string(thisRid);
            out.push_back(std::move(r));
        }
        return out;
    }

    const auto* res = store.find(oid, static_cast<std::uint32_t>(iid),
                                       static_cast<std::uint32_t>(rid));
    if (!res) return out;

    LinkEntry e;
    e.uri = "/" + std::to_string(oid) + "/" + std::to_string(iid)
          + "/" + std::to_string(rid);

    // D2: per-resource notification attributes are keyed by Short Server ID.
    // For v1 we look up the row matching `shortServerId`; if none exists
    // the resource is emitted without attributes.
    for (const auto& a : res->attrs) {
        if (a.shortServerId != shortServerId) continue;
        if (a.pmin > 0)        e.set("pmin", static_cast<std::uint64_t>(a.pmin));
        if (a.pmax > 0)        e.set("pmax", static_cast<std::uint64_t>(a.pmax));
        if (a.hasGt) {
            std::ostringstream s; s << a.gt; e.set("gt", s.str(), false);
        }
        if (a.hasLt) {
            std::ostringstream s; s << a.lt; e.set("lt", s.str(), false);
        }
        if (a.hasSt) {
            std::ostringstream s; s << a.st; e.set("st", s.str(), false);
        }
        break;   // single-server v1: only one matching row expected
    }

    if (res->observable) {
        e.set_flag("obs");
    }
    out.push_back(std::move(e));
    return out;
}

}} // namespace lwm2m::linkformat
