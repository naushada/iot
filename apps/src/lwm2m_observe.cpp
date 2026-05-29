#include "lwm2m_observe.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "lwm2m_coap_builder.hpp"

namespace lwm2m {

using namespace ::lwm2m::coap;

/* ───────────────────────── helpers ────────────────────────────────── */

namespace {

std::string to_hex(const std::string& bytes) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(k[c >> 4]);
        out.push_back(k[c & 0xF]);
    }
    return out;
}

/// Try to parse `s` as a double. Returns false on garbage.
bool try_parse_double(const std::string& s, double& out) {
    try {
        std::size_t consumed = 0;
        out = std::stod(s, &consumed);
        // Consider trailing whitespace harmless.
        while (consumed < s.size() && std::isspace(static_cast<unsigned char>(s[consumed]))) ++consumed;
        return consumed == s.size();
    } catch (...) {
        return false;
    }
}

bool same_value(const std::string& a, const std::string& b) {
    return a == b;
}

} // namespace

/* ───────────────────────── threshold engine ───────────────────────── */

NotifyDecision evaluate(const EngineInput& in) {
    if (!in.observer || !in.newValue) return NotifyDecision::Skip;

    const auto& obs = *in.observer;
    const auto& a   = obs.attrs;

    // No prior value → the initial 2.05 is the caller's job; never fire
    // an extra notify in that window.
    if (!obs.lastValue.has_value()) {
        // Treat as "first change visible"; if pmax is set we'll catch it
        // via due_at(); otherwise the next change triggers normally.
        return NotifyDecision::EmitNow;
    }

    // pmin: do not fire if too soon since the last notify.
    if (a.pmin > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 in.now - obs.lastSentAt).count();
        if (elapsed < static_cast<std::int64_t>(a.pmin)) {
            return NotifyDecision::Defer;
        }
    }

    const bool valueDiffers = !same_value(*obs.lastValue, *in.newValue);

    // Numeric threshold checks. If parsing fails for either side, fall
    // back to "any difference fires" — string/opaque case.
    if (a.hasGt || a.hasLt || a.hasSt) {
        double oldD = 0, newD = 0;
        if (try_parse_double(*obs.lastValue, oldD) &&
            try_parse_double(*in.newValue,   newD)) {
            bool cross = false;
            if (a.hasGt) {
                if ((oldD <= a.gt && newD > a.gt) ||
                    (oldD >  a.gt && newD <= a.gt)) cross = true;
            }
            if (a.hasLt) {
                if ((oldD >= a.lt && newD < a.lt) ||
                    (oldD <  a.lt && newD >= a.lt)) cross = true;
            }
            if (a.hasSt) {
                if (std::abs(newD - oldD) >= a.st) cross = true;
            }
            return cross ? NotifyDecision::EmitNow : NotifyDecision::Skip;
        }
        // Non-numeric value with numeric thresholds set → fall through.
    }

    return valueDiffers ? NotifyDecision::EmitNow : NotifyDecision::Skip;
}

/* ───────────────────────── notify frame ───────────────────────────── */

bool confirmable_for(const ObserverContext& obs) {
    if (obs.observeCritical) return true;
    // After every Nth notify, promote the next one to CON. notifyCount
    // increments at frame build time, so we test (notifyCount + 1) here.
    const std::uint32_t next = obs.notifyCount + 1;
    return next > 0 && (next % kConDeadPeerInterval) == 0;
}

std::string build_notify_frame(const NotifyFrame& f) {
    std::ostringstream ss;
    emit_header(ss,
                f.messageId,
                f.observer ? f.observer->token : std::string{},
                RSP_205_CONTENT,
                f.confirmable ? TYPE_CON : TYPE_NON);

    std::uint16_t prev = 0;

    // Observe option (number 6). 24-bit big-endian; minimal length.
    if (f.observer) {
        std::string obsVal;
        std::uint32_t s = f.observer->seq & 0x00FFFFFFu;
        if (s == 0) {
            obsVal.assign("");                   // 0-byte = 0
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
        emit_option(ss, /*Observe = 6*/ 6, obsVal, prev);
    }

    emit_option(ss, OPT_CONTENT_FORMAT, cf_bytes(f.contentFormat), prev);
    if (!f.payload.empty()) {
        ss.put(static_cast<char>(0xFF));
        ss << f.payload;
    }
    return ss.str();
}

/* ───────────────────────── registry ───────────────────────────────── */

ObserverRegistry::Key
ObserverRegistry::make_key(const std::string& peer, const std::string& token) {
    return peer + "|" + to_hex(token);
}

ObserverContext& ObserverRegistry::add(ObserverContext ctx) {
    auto key = make_key(ctx.peer, ctx.token);
    auto it  = m_byKey.find(key);
    if (it == m_byKey.end()) {
        ctx.notifyCount = 0;
        ctx.seq         = 0;
        auto [ins, _]   = m_byKey.emplace(std::move(key), std::move(ctx));
        return ins->second;
    }
    // Re-Observe: refresh attrs / observable flag, preserve seq + count
    // so RFC 7641 §3.4 monotonicity holds.
    it->second.shortServerId   = ctx.shortServerId;
    it->second.oid             = ctx.oid;
    it->second.iid             = ctx.iid;
    it->second.rid             = ctx.rid;
    it->second.hasIid          = ctx.hasIid;
    it->second.hasRid          = ctx.hasRid;
    it->second.attrs           = ctx.attrs;
    it->second.observeCritical = ctx.observeCritical;
    return it->second;
}

bool ObserverRegistry::remove(const std::string& peer, const std::string& token) {
    return m_byKey.erase(make_key(peer, token)) > 0;
}

std::size_t ObserverRegistry::remove_peer(const std::string& peer) {
    std::size_t n = 0;
    const std::string prefix = peer + "|";
    for (auto it = m_byKey.begin(); it != m_byKey.end(); ) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = m_byKey.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

ObserverContext* ObserverRegistry::find(const std::string& peer,
                                        const std::string& token) {
    auto it = m_byKey.find(make_key(peer, token));
    return it == m_byKey.end() ? nullptr : &it->second;
}

const ObserverContext* ObserverRegistry::find(const std::string& peer,
                                              const std::string& token) const {
    auto it = m_byKey.find(make_key(peer, token));
    return it == m_byKey.end() ? nullptr : &it->second;
}

std::vector<ObserverContext*>
ObserverRegistry::targeting(std::uint32_t oid,
                            std::uint32_t iid,
                            std::uint32_t rid) {
    std::vector<ObserverContext*> out;
    for (auto& [_, ctx] : m_byKey) {
        if (ctx.oid != oid) continue;
        // Whole-object observer (no iid) covers any matching oid.
        // Whole-instance observer (no rid) covers any rid of that instance.
        if (ctx.hasIid && ctx.iid != iid) continue;
        if (ctx.hasRid && ctx.rid != rid) continue;
        out.push_back(&ctx);
    }
    return out;
}

std::vector<ObserverContext*>
ObserverRegistry::due_at(std::chrono::steady_clock::time_point now) {
    std::vector<ObserverContext*> out;
    for (auto& [_, ctx] : m_byKey) {
        if (ctx.attrs.pmax == 0) continue;          // pmax unset → never due
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - ctx.lastSentAt).count();
        if (elapsed >= static_cast<std::int64_t>(ctx.attrs.pmax)) {
            out.push_back(&ctx);
        }
    }
    return out;
}

} // namespace lwm2m
