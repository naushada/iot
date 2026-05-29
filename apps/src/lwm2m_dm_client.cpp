#include "lwm2m_dm_client.hpp"

#include <cctype>
#include <chrono>
#include <sstream>

#include "lwm2m_coap_builder.hpp"
#include "lwm2m_codec_linkformat.hpp"
#include "lwm2m_codec_opaque.hpp"
#include "lwm2m_codec_plaintext.hpp"
#include "lwm2m_codec_registry.hpp"
#include "lwm2m_codec_tlv.hpp"
#include "lwm2m_observe.hpp"

namespace lwm2m {

using namespace ::lwm2m::coap;

namespace {

/// Parse "/{oid}[/{iid}[/{rid}[/{riid}]]]" into the four ints. Each is
/// -1 when absent. Returns false for anything that is not a numeric DM
/// path (anything starting with /bs, /rd, /push, etc. fails fast here).
bool parse_dm_uri(const std::string& uri,
                  std::int32_t& oid, std::int32_t& iid,
                  std::int32_t& rid, std::int32_t& riid) {
    oid = iid = rid = riid = -1;
    if (uri.empty() || uri[0] != '/') return false;
    std::size_t i = 1;
    auto take = [&](std::int32_t& out) -> bool {
        std::size_t start = i;
        while (i < uri.size() && uri[i] != '/') ++i;
        if (i == start) return false;
        for (std::size_t j = start; j < i; ++j) {
            if (!std::isdigit(static_cast<unsigned char>(uri[j]))) return false;
        }
        out = std::stoi(uri.substr(start, i - start));
        return true;
    };
    if (!take(oid)) return false;
    if (i < uri.size() && uri[i] == '/') { ++i; if (!take(iid)) return false; }
    if (i < uri.size() && uri[i] == '/') { ++i; if (!take(rid)) return false; }
    if (i < uri.size() && uri[i] == '/') { ++i; if (!take(riid)) return false; }
    return true;
}

/// Extract the Accept option value (single 1- or 2-byte integer).
std::int32_t get_accept(const CoAPAdapter::CoAPMessage& msg, CoAPAdapter& coap) {
    for (const auto& opt : msg.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) != "Accept") continue;
        if (opt.optionvalue.size() == 1) {
            return static_cast<std::uint8_t>(opt.optionvalue[0]);
        }
        if (opt.optionvalue.size() == 2) {
            return (static_cast<std::uint8_t>(opt.optionvalue[0]) << 8) |
                    static_cast<std::uint8_t>(opt.optionvalue[1]);
        }
    }
    return -1;
}

/// Observe option (number 6). Returns:
///   -1  → option absent
///    0  → register (initial Observe request)
///    1  → cancel
///  other → treat as register (value is ignored on register per RFC 7641)
std::int32_t get_observe(const CoAPAdapter::CoAPMessage& msg) {
    for (const auto& opt : msg.uripath) {
        if (opt.optiondelta != 6) continue;
        if (opt.optionvalue.empty()) return 0;
        std::uint32_t v = 0;
        for (unsigned char c : opt.optionvalue) v = (v << 8) | c;
        if (v == 1) return 1;
        return 0;
    }
    return -1;
}

/// Same shape for the Content-Format option.
std::int32_t get_cf(const CoAPAdapter::CoAPMessage& msg, CoAPAdapter& coap) {
    for (const auto& opt : msg.uripath) {
        if (coap.getOptionNumber(opt.optiondelta) != "Content-Format") continue;
        if (opt.optionvalue.size() == 1) {
            return static_cast<std::uint8_t>(opt.optionvalue[0]);
        }
        if (opt.optionvalue.size() == 2) {
            return (static_cast<std::uint8_t>(opt.optionvalue[0]) << 8) |
                    static_cast<std::uint8_t>(opt.optionvalue[1]);
        }
    }
    return -1;
}

bool is_write_attributes_query(const CoAPAdapter::CoAPMessage& msg,
                               CoAPAdapter& coap) {
    static const char* const k[] = {"pmin", "pmax", "gt", "lt", "st"};
    for (const auto& key : k) {
        if (!get_query(msg, coap, key).empty()) return true;
    }
    return false;
}

/// Build a 2.05 Content response carrying CF + payload, with an optional
/// Observe sequence number (RFC 7641 §3) that, when present, must be
/// emitted before Content-Format because the option-number is lower.
std::string build_content(const CoAPAdapter::CoAPMessage& msg,
                          std::uint16_t cf, const std::string& body,
                          bool withObserve = false,
                          std::uint32_t observeSeq = 0) {
    std::ostringstream ss;
    emit_header(ss, msg.coapheader.msgid,
                std::string(msg.tokens.begin(), msg.tokens.end()),
                RSP_205_CONTENT, TYPE_ACK);
    std::uint16_t prev = 0;
    if (withObserve) {
        std::string obsVal;
        std::uint32_t s = observeSeq & 0x00FFFFFFu;
        if (s == 0) {
            // 0-byte option value per CoAP option encoding rules.
        } else if (s <= 0xFF) {
            obsVal.push_back(static_cast<char>(s));
        } else if (s <= 0xFFFF) {
            obsVal.push_back(static_cast<char>((s >> 8) & 0xFF));
            obsVal.push_back(static_cast<char>(s & 0xFF));
        } else {
            obsVal.push_back(static_cast<char>((s >> 16) & 0xFF));
            obsVal.push_back(static_cast<char>((s >>  8) & 0xFF));
            obsVal.push_back(static_cast<char>(s        & 0xFF));
        }
        emit_option(ss, /*Observe*/ 6, obsVal, prev);
    }
    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(cf), prev);
    if (!body.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << body;
    }
    return ss.str();
}

/// Build 2.01 Created with Location-Path describing the new instance.
std::string build_created_with_path(const CoAPAdapter::CoAPMessage& msg,
                                    std::uint32_t oid, std::uint32_t iid) {
    std::ostringstream ss;
    emit_header(ss, msg.coapheader.msgid,
                std::string(msg.tokens.begin(), msg.tokens.end()),
                RSP_201_CREATED, TYPE_ACK);
    std::uint16_t prev = 0;
    emit_option(ss, OPT_LOCATION_PATH, std::to_string(oid), prev);
    emit_option(ss, OPT_LOCATION_PATH, std::to_string(iid), prev);
    return ss.str();
}

/// Pick the encode CF for a single Resource: opaque for binary, plain
/// text otherwise.
std::uint16_t default_encode_cf(ResourceType t) {
    switch (t) {
        case ResourceType::Opaque:
        case ResourceType::None:
            return CF_OctetStream;       // 42
        default:
            return CF_PlainText;         // 0
    }
}

/// Apply a single Write-Attributes parameter to a row keyed by ssid.
/// `numericFn` parses a positive integer (`pmin`/`pmax`) or a double
/// (`gt`/`lt`/`st`); returns true on success.
bool apply_attribute(NotificationAttributes& row,
                     const std::string& key, const std::string& value) {
    try {
        if (key == "pmin") { row.pmin = static_cast<std::uint32_t>(std::stoul(value)); return true; }
        if (key == "pmax") { row.pmax = static_cast<std::uint32_t>(std::stoul(value)); return true; }
        if (key == "gt")   { row.gt = std::stod(value); row.hasGt = true; return true; }
        if (key == "lt")   { row.lt = std::stod(value); row.hasLt = true; return true; }
        if (key == "st")   { row.st = std::stod(value); row.hasSt = true; return true; }
    } catch (...) { /* fall through to false */ }
    return false;
}

NotificationAttributes& find_or_create_row(Resource& r, std::uint16_t ssid) {
    for (auto& a : r.attrs) if (a.shortServerId == ssid) return a;
    NotificationAttributes fresh;
    fresh.shortServerId = ssid;
    r.attrs.push_back(fresh);
    return r.attrs.back();
}

/* ───────────────────────── Read (REQ-DM-001 / REQ-DM-002) ───────────────── */

std::string encode_single_resource(const Resource& res,
                                   std::int32_t acceptCf) {
    std::string raw;
    if (res.read) raw = res.read();

    if (acceptCf == CF_OctetStream) {
        std::string out;
        ::lwm2m::opaque::encode(raw, out);
        return out;
    }
    // Default: plain text per the resource type.
    std::string out;
    ::lwm2m::plaintext::encode(res.type, raw, out);
    return out;
}

DmOutcome handle_read(const CoAPAdapter::CoAPMessage& msg,
                      CoAPAdapter& coap,
                      ObjectStore& store,
                      std::uint16_t ssid,
                      std::uint32_t oid,
                      std::int32_t iid,
                      std::int32_t rid) {
    DmOutcome out;
    const std::int32_t accept = get_accept(msg, coap);

    // Discover variant.
    if (accept == CF_LinkFormat) {
        auto entries = ::lwm2m::linkformat::discover(
            store, oid, iid, rid, ssid);
        if (entries.empty()) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        out.kind = DmOutcome::Discover;
        out.response = build_content(msg, CF_LinkFormat,
                                     ::lwm2m::linkformat::encode(entries));
        return out;
    }

    // Single-resource Read.
    if (iid >= 0 && rid >= 0) {
        auto* r = store.find(oid, iid, rid);
        if (!r) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        if (!r->read || !has_op(r->ops, Operations::R)) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_405_METHOD);
            return out;
        }
        std::uint16_t cf = (accept >= 0) ? static_cast<std::uint16_t>(accept)
                                          : default_encode_cf(r->type);
        std::string body = encode_single_resource(*r, cf);
        out.kind = DmOutcome::Read;
        out.response = build_content(msg, cf, body);
        return out;
    }

    // Whole-instance / whole-object Read → TLV container.
    if (iid >= 0) {
        auto* inst = store.find(oid, iid);
        if (!inst) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        std::string body;
        for (const auto& [thisRid, res] : inst->resources) {
            if (!res.read || !has_op(res.ops, Operations::R)) continue;
            std::string rec;
            ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11,
                                        res.read(),
                                        static_cast<std::uint16_t>(thisRid),
                                        rec);
            body += rec;
        }
        out.kind = DmOutcome::Read;
        out.response = build_content(msg, CF_LwM2MTlv, body);
        return out;
    }

    auto* desc = store.find(oid);
    if (!desc) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    // Whole-object Read is a TLV container of Object Instances.
    std::string body;
    for (const auto& [thisIid, inst] : desc->instances) {
        std::string instBody;
        for (const auto& [thisRid, res] : inst.resources) {
            if (!res.read || !has_op(res.ops, Operations::R)) continue;
            std::string rec;
            ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11,
                                        res.read(),
                                        static_cast<std::uint16_t>(thisRid),
                                        rec);
            instBody += rec;
        }
        std::string instRec;
        ::lwm2m::tlv::encode_string(TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00,
                                    instBody,
                                    static_cast<std::uint16_t>(thisIid),
                                    instRec);
        body += instRec;
    }
    out.kind = DmOutcome::Read;
    out.response = build_content(msg, CF_LwM2MTlv, body);
    return out;
}

/* ───────────────────────── Write (REQ-DM-003) ─────────────────────────── */

DmOutcome handle_write(const CoAPAdapter::CoAPMessage& msg,
                       CoAPAdapter& coap,
                       ObjectStore& store,
                       std::uint8_t method,
                       std::uint32_t oid,
                       std::int32_t iid,
                       std::int32_t rid) {
    DmOutcome out;
    if (iid < 0) {
        // /{oid} is Create, handled separately.
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }
    auto* inst = store.find(oid, iid);
    if (!inst) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    const std::int32_t cf = get_cf(msg, coap);

    // Single-resource Write.
    if (rid >= 0) {
        auto it = inst->resources.find(static_cast<std::uint32_t>(rid));
        if (it == inst->resources.end()) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        auto& r = it->second;
        if (!r.write || !has_op(r.ops, Operations::W)) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_405_METHOD);
            return out;
        }
        std::string decoded;
        int rc = 0;
        if (cf < 0 || cf == CF_PlainText) {
            rc = ::lwm2m::plaintext::decode(r.type, msg.payload, decoded);
        } else if (cf == CF_OctetStream) {
            rc = ::lwm2m::opaque::decode(msg.payload, decoded);
        } else if (cf == CF_LwM2MTlv) {
            // Take the first ResourceWithValue record's value.
            LwM2MObject obj;
            LwM2MObjectData scratch;
            if (::lwm2m::tlv::decode_container(msg.payload, scratch, obj) != 0 ||
                obj.m_value.empty()) {
                rc = -1;
            } else {
                decoded.assign(obj.m_value.front().m_ridvalue.begin(),
                               obj.m_value.front().m_ridvalue.end());
            }
        } else {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_415_UNSUP_CF);
            return out;
        }
        if (rc != 0) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_400_BAD_REQ);
            return out;
        }
        int wr = r.write(decoded);
        out.kind = DmOutcome::Write;
        out.response = build_ack(msg,
            wr == 0 ? RSP_204_CHANGED : RSP_500_INTERNAL);
        return out;
    }

    // Whole-instance Write — TLV container only. PUT replaces (clears
    // prior resources first); POST is partial update.
    if (cf >= 0 && cf != CF_LwM2MTlv) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_415_UNSUP_CF);
        return out;
    }
    LwM2MObject obj;
    LwM2MObjectData scratch;
    if (::lwm2m::tlv::decode_container(msg.payload, scratch, obj) != 0) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }

    bool replace = (method == METHOD_PUT);
    if (replace) {
        for (auto& [_thisRid, res] : inst->resources) {
            if (res.write && has_op(res.ops, Operations::W)) {
                res.write(std::string{});
            }
        }
    }
    for (const auto& rec : obj.m_value) {
        auto it = inst->resources.find(rec.m_rid);
        if (it == inst->resources.end()) continue;
        auto& r = it->second;
        if (!r.write || !has_op(r.ops, Operations::W)) continue;
        std::string v(rec.m_ridvalue.begin(), rec.m_ridvalue.end());
        r.write(v);
    }
    out.kind = DmOutcome::Write;
    out.response = build_ack(msg, RSP_204_CHANGED);
    return out;
}

/* ───────────────────────── Create (REQ-DM-004) ────────────────────────── */

DmOutcome handle_create(const CoAPAdapter::CoAPMessage& msg,
                        CoAPAdapter& coap,
                        ObjectStore& store,
                        std::uint32_t oid) {
    DmOutcome out;
    auto* desc = store.find(oid);
    if (!desc) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    if (!desc->multipleInstance) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_405_METHOD);
        return out;
    }
    const std::int32_t cf = get_cf(msg, coap);
    if (cf >= 0 && cf != CF_LwM2MTlv) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_415_UNSUP_CF);
        return out;
    }

    LwM2MObject obj;
    LwM2MObjectData scratch;
    if (::lwm2m::tlv::decode_container(msg.payload, scratch, obj) != 0) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }

    std::uint32_t newIid = 0;
    if (!desc->instances.empty()) newIid = desc->instances.rbegin()->first + 1;

    ObjectInstance fresh;
    fresh.iid = newIid;
    // Inherit the descriptor's resource templates so each new instance
    // ships with the right type / ops / observable metadata.
    fresh.resources = desc->resourceTemplates;

    for (const auto& rec : obj.m_value) {
        auto it = fresh.resources.find(rec.m_rid);
        if (it == fresh.resources.end()) continue;
        auto& r = it->second;
        if (!r.write || !has_op(r.ops, Operations::W)) continue;
        std::string v(rec.m_ridvalue.begin(), rec.m_ridvalue.end());
        r.write(v);
    }

    desc->instances[newIid] = std::move(fresh);
    out.kind = DmOutcome::Create;
    out.response = build_created_with_path(msg, oid, newIid);
    return out;
}

/* ───────────────────────── Delete (REQ-DM-005) ────────────────────────── */

DmOutcome handle_delete(const CoAPAdapter::CoAPMessage& msg,
                        ObjectStore& store,
                        std::uint32_t oid,
                        std::int32_t iid) {
    DmOutcome out;
    if (iid < 0) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }
    auto* desc = store.find(oid);
    if (!desc || !desc->instances.count(iid)) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    desc->instances.erase(iid);
    out.kind = DmOutcome::Delete;
    out.response = build_ack(msg, RSP_202_DELETED);
    return out;
}

/* ───────────────────────── Execute (REQ-DM-006) ───────────────────────── */

DmOutcome handle_execute(const CoAPAdapter::CoAPMessage& msg,
                         ObjectStore& store,
                         std::uint32_t oid,
                         std::int32_t iid,
                         std::int32_t rid) {
    DmOutcome out;
    if (iid < 0 || rid < 0) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_400_BAD_REQ);
        return out;
    }
    auto* r = store.find(oid, iid, rid);
    if (!r) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    if (!r->execute || !has_op(r->ops, Operations::E)) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_405_METHOD);
        return out;
    }
    int rc = r->execute(msg.payload);
    out.kind = DmOutcome::Execute;
    out.response = build_ack(msg,
        rc == 0 ? RSP_204_CHANGED : RSP_500_INTERNAL);
    return out;
}

/* ───────────────────────── Write-Attributes (REQ-DM-007) ──────────────── */

DmOutcome handle_write_attributes(const CoAPAdapter::CoAPMessage& msg,
                                  CoAPAdapter& coap,
                                  ObjectStore& store,
                                  std::uint16_t ssid,
                                  std::uint32_t oid,
                                  std::int32_t iid,
                                  std::int32_t rid) {
    DmOutcome out;
    auto* desc = store.find(oid);
    if (!desc) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }

    auto apply_to = [&](Resource& r) -> bool {
        auto& row = find_or_create_row(r, ssid);
        bool ok = true;
        for (auto key : {"pmin","pmax","gt","lt","st"}) {
            auto v = get_query(msg, coap, key);
            if (v.empty()) continue;
            ok = ok && apply_attribute(row, key, v);
        }
        return ok;
    };

    if (iid >= 0 && rid >= 0) {
        auto* r = store.find(oid, iid, rid);
        if (!r) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        if (!apply_to(*r)) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_400_BAD_REQ);
            return out;
        }
    } else if (iid >= 0) {
        auto* inst = store.find(oid, iid);
        if (!inst) {
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_404_NOT_FND);
            return out;
        }
        for (auto& [_thisRid, r] : inst->resources) (void)apply_to(r);
    } else {
        for (auto& [_thisIid, inst] : desc->instances) {
            for (auto& [_thisRid, r] : inst.resources) (void)apply_to(r);
        }
    }

    out.kind = DmOutcome::WriteAttributes;
    out.response = build_ack(msg, RSP_204_CHANGED);
    return out;
}

} // anonymous namespace

/* ───────────────────────── DmClient impl ──────────────────────────────── */

DmClient::DmClient(std::shared_ptr<ObjectStore> store)
    : m_store(std::move(store)) {}

namespace {

/// Snapshot the current notification-attributes row for `(resource, ssid)`,
/// or return an empty default if none exists yet (Write-Attributes never
/// called).
NotificationAttributes snapshot_attrs(const Resource& r, std::uint16_t ssid) {
    for (const auto& a : r.attrs) if (a.shortServerId == ssid) return a;
    NotificationAttributes def;
    def.shortServerId = ssid;
    return def;
}

/// Encode a fresh resource value for the wire. Mirrors the Read path:
/// plain text for non-opaque types, opaque otherwise.
std::pair<std::uint16_t, std::string>
encode_for_notify(const Resource& r, const std::string& raw) {
    if (r.type == ResourceType::Opaque || r.type == ResourceType::None) {
        std::string out; ::lwm2m::opaque::encode(raw, out);
        return {CF_OctetStream, out};
    }
    std::string out; ::lwm2m::plaintext::encode(r.type, raw, out);
    return {CF_PlainText, out};
}

} // namespace

DmOutcome DmClient::handle_observe_register(const CoAPAdapter::CoAPMessage& msg,
                                            CoAPAdapter& coap) {
    DmOutcome out;
    std::int32_t oid = -1, iid = -1, rid = -1, riid = -1;
    parse_dm_uri(join_uri(msg, coap), oid, iid, rid, riid);

    auto* res = m_store->find(static_cast<std::uint32_t>(oid),
                              static_cast<std::uint32_t>(iid),
                              static_cast<std::uint32_t>(rid));
    if (!res) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_404_NOT_FND);
        return out;
    }
    if (!res->read || !has_op(res->ops, Operations::R)) {
        out.kind = DmOutcome::Error;
        out.response = build_ack(msg, RSP_405_METHOD);
        return out;
    }

    ObserverContext ctx;
    ctx.shortServerId = m_callerSsid;
    ctx.peer          = m_callerPeer;
    ctx.token.assign(msg.tokens.begin(), msg.tokens.end());
    ctx.oid           = static_cast<std::uint32_t>(oid);
    ctx.iid           = static_cast<std::uint32_t>(iid);
    ctx.rid           = static_cast<std::uint32_t>(rid);
    ctx.hasIid        = true;
    ctx.hasRid        = true;
    ctx.attrs         = snapshot_attrs(*res, m_callerSsid);
    ctx.observeCritical = res->observable;     // initial heuristic; D4 hook
    ctx.lastSentAt    = std::chrono::steady_clock::now();

    auto& stored = m_observers.add(std::move(ctx));

    // Initial response carries the current value + Observe: 0.
    std::string raw = res->read();
    auto [cf, body] = encode_for_notify(*res, raw);
    stored.lastValue = raw;

    out.kind = DmOutcome::Observe;
    out.response = build_content(msg, cf, body,
                                 /*withObserve*/ true,
                                 /*seq*/ stored.seq);
    return out;
}

DmOutcome DmClient::handle_observe_cancel(const CoAPAdapter::CoAPMessage& msg) {
    DmOutcome out;
    std::string token(msg.tokens.begin(), msg.tokens.end());
    bool removed = m_observers.remove(m_callerPeer, token);
    out.kind = DmOutcome::ObserveCancel;
    out.response = build_ack(msg, removed ? RSP_205_CONTENT : RSP_404_NOT_FND);
    return out;
}

std::vector<std::string>
DmClient::on_resource_changed(std::uint32_t oid,
                              std::uint32_t iid,
                              std::uint32_t rid,
                              const std::string& newValue) {
    std::vector<std::string> frames;
    auto* res = m_store->find(oid, iid, rid);
    if (!res) return frames;

    auto matching = m_observers.targeting(oid, iid, rid);
    auto now = std::chrono::steady_clock::now();

    for (auto* obs : matching) {
        EngineInput in;
        in.observer = obs;
        in.newValue = &newValue;
        in.now      = now;
        auto verdict = evaluate(in);
        if (verdict != NotifyDecision::EmitNow) continue;

        auto [cf, body] = encode_for_notify(*res, newValue);
        NotifyFrame f;
        f.observer       = obs;
        f.payload        = body;
        f.contentFormat  = cf;
        f.messageId      = next_notify_msgid();
        f.confirmable    = confirmable_for(*obs);
        obs->seq         = (obs->seq + 1) & 0x00FFFFFFu;
        obs->notifyCount = obs->notifyCount + 1;
        obs->lastSentAt  = now;
        obs->lastValue   = newValue;
        frames.push_back(build_notify_frame(f));
    }
    return frames;
}

std::vector<std::string>
DmClient::tick(std::chrono::steady_clock::time_point now) {
    std::vector<std::string> frames;
    for (auto* obs : m_observers.due_at(now)) {
        auto* res = m_store->find(obs->oid, obs->iid, obs->rid);
        if (!res || !res->read) continue;
        std::string raw = res->read();
        auto [cf, body] = encode_for_notify(*res, raw);
        NotifyFrame f;
        f.observer      = obs;
        f.payload       = body;
        f.contentFormat = cf;
        f.messageId     = next_notify_msgid();
        f.confirmable   = confirmable_for(*obs);
        obs->seq        = (obs->seq + 1) & 0x00FFFFFFu;
        obs->notifyCount += 1;
        obs->lastSentAt = now;
        obs->lastValue  = raw;
        frames.push_back(build_notify_frame(f));
    }
    return frames;
}

std::size_t DmClient::on_rst_from(const std::string& peer) {
    return m_observers.remove_peer(peer);
}

DmOutcome DmClient::handle(const CoAPAdapter::CoAPMessage& msg,
                           CoAPAdapter& coap) {
    DmOutcome none;
    if (!m_store) return none;

    const std::string uri = join_uri(msg, coap);
    std::int32_t oid = -1, iid = -1, rid = -1, riid = -1;
    if (!parse_dm_uri(uri, oid, iid, rid, riid)) return none;
    if (oid < 0) return none;

    const std::uint8_t method = msg.coapheader.code & 0x1F;

    switch (method) {
        case METHOD_GET: {
            // L7: Observe registration / cancel takes precedence over Read.
            const std::int32_t obs = get_observe(msg);
            if (obs == 0 && iid >= 0 && rid >= 0) {
                return handle_observe_register(msg, coap);
            }
            if (obs == 1) {
                return handle_observe_cancel(msg);
            }
            return handle_read(msg, coap, *m_store, m_callerSsid,
                               static_cast<std::uint32_t>(oid), iid, rid);
        }
        case METHOD_PUT:
            if (is_write_attributes_query(msg, coap) && msg.payload.empty()) {
                return handle_write_attributes(msg, coap, *m_store,
                                               m_callerSsid,
                                               static_cast<std::uint32_t>(oid),
                                               iid, rid);
            }
            return handle_write(msg, coap, *m_store, METHOD_PUT,
                                static_cast<std::uint32_t>(oid), iid, rid);
        case METHOD_POST:
            if (iid < 0) {
                return handle_create(msg, coap, *m_store,
                                     static_cast<std::uint32_t>(oid));
            }
            if (rid >= 0) {
                return handle_execute(msg, *m_store,
                                      static_cast<std::uint32_t>(oid),
                                      iid, rid);
            }
            return handle_write(msg, coap, *m_store, METHOD_POST,
                                static_cast<std::uint32_t>(oid), iid, rid);
        case METHOD_DELETE:
            return handle_delete(msg, *m_store,
                                 static_cast<std::uint32_t>(oid), iid);
        default: {
            DmOutcome out;
            out.kind = DmOutcome::Error;
            out.response = build_ack(msg, RSP_405_METHOD);
            return out;
        }
    }
}

} // namespace lwm2m
