#ifndef __lwm2m_observe_hpp__
#define __lwm2m_observe_hpp__

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_observe.hpp
 * @brief Information Reporting (Observe / Notify) for LwM2M.
 *
 * L7 deliverable per design §5.5 and RDD REQ-IR-001..005.
 *
 * Decision D4 baked in:
 *   - Notify defaults to Non-Confirmable.
 *   - Every Nth (`kConDeadPeerInterval`, default 10) notify per observer is
 *     promoted to Confirmable so the server can detect a dead peer.
 *   - `Resource::observeCritical` (added in this header) forces CON.
 *
 * Pure logic. The notify byte string is produced by the engine; the caller
 * (DmClient) ships it through the same outbound queue used for ACKs.
 */

namespace lwm2m {

/// Per-RDD D4: how often a NON observer gets a CON-promoted notify to
/// catch a dead peer without waiting for pmax skew.
constexpr std::uint32_t kConDeadPeerInterval = 10;

/// Critical-observation marker. Lives on `Resource` so the engine doesn't
/// need a separate side-table. False is the spec default.
/// (Added here rather than mutating `lwm2m_object_store.hpp` because the
/// store header is consumed by L1 tests that don't link the observe
/// module; this struct holds the L7 view of a Resource.)
struct ObserveAttributes {
    bool observeCritical{false};
};

/// One active Observe relationship, keyed by (peer, token).
struct ObserverContext {
    std::uint16_t  shortServerId{1};   ///< D2 / used for attribute lookup
    std::string    peer;               ///< "host:port" key, opaque to engine
    std::string    token;              ///< CoAP token bytes (raw)
    std::uint32_t  oid{0};
    std::uint32_t  iid{0};
    std::uint32_t  rid{0};
    bool           hasIid{true};
    bool           hasRid{true};

    /// Last value successfully serialised onto the wire.
    std::optional<std::string> lastValue;
    /// 24-bit Observe sequence number per RFC 7641 §3.4. Wraps after 2^24-1.
    std::uint32_t  seq{0};
    /// How many notifies (excluding the initial 2.05) we have shipped.
    std::uint32_t  notifyCount{0};
    /// Wall-clock-monotonic stamp of the last notify (or initial response).
    std::chrono::steady_clock::time_point lastSentAt{};

    /// Per-Observer copy of the resource's notification attributes,
    /// snapshotted at Observe-start so a subsequent Write-Attributes does
    /// not retroactively change this observer's pmin/pmax mid-window.
    NotificationAttributes attrs;
    /// Critical flag mirrored from the resource at Observe time.
    bool observeCritical{false};
};

/// Verdict of the threshold engine when a value change is presented.
enum class NotifyDecision : std::uint8_t {
    Skip,        ///< Conditions not met (e.g. value didn't cross a threshold).
    Defer,       ///< Would notify but pmin guard rejects it; caller retries on next pmax tick.
    EmitNow,     ///< Build and ship a notify now.
};

/// Inputs to the threshold engine. Numeric fields are parsed from the
/// resource value as needed; non-numeric resources fall back to "any
/// change emits" semantics (string / opaque comparison).
struct EngineInput {
    const ObserverContext*       observer{nullptr};
    const std::string*           newValue{nullptr};
    std::chrono::steady_clock::time_point now{};
};

/// Decide whether a Notify should fire given the observer's attrs and
/// the time since `observer->lastSentAt`. Pure function; no I/O.
NotifyDecision evaluate(const EngineInput& in);

/// Outbound notify request. The caller turns this into CoAP bytes via
/// `build_notify_frame()` below.
struct NotifyFrame {
    const ObserverContext* observer{nullptr};
    std::string            payload;        ///< Plain text / opaque / TLV — codec decided by caller
    std::uint16_t          contentFormat{0};
    std::uint16_t          messageId{0};
    bool                   confirmable{false};
};

/// Encode `NotifyFrame` into CoAP bytes per RFC 7641 §3:
///   `2.05 Content` + `Observe: seq` + `Content-Format` + payload.
/// D4 policy: caller sets `confirmable=true` based on the
/// `confirmable_for(observer)` helper before invoking.
std::string build_notify_frame(const NotifyFrame& f);

/// D4 policy gate. Returns true when this observer's next notify should
/// be promoted to CON. Increments are done inside the engine, not here.
bool confirmable_for(const ObserverContext& obs);

/// Track active observers across the device. One instance per
/// `ObjectStore` (i.e. one per client endpoint on the device side).
class ObserverRegistry {
public:
    using Key = std::string;     ///< "peer|token-hex"

    /// Add or refresh an observer (re-Observe re-snapshots attrs).
    /// Returns a reference to the stored context.
    ObserverContext& add(ObserverContext ctx);

    /// Remove by (peer, token). Returns true on success.
    bool remove(const std::string& peer, const std::string& token);

    /// Remove every observer matching `peer`. Used on RST reception.
    std::size_t remove_peer(const std::string& peer);

    /// Lookup. nullptr if absent.
    ObserverContext*       find(const std::string& peer, const std::string& token);
    const ObserverContext* find(const std::string& peer, const std::string& token) const;

    /// All observers whose URI addresses (`oid`,`iid`,`rid`). Use after
    /// a `Resource::write` lands to evaluate and emit notifies.
    std::vector<ObserverContext*> targeting(std::uint32_t oid,
                                            std::uint32_t iid,
                                            std::uint32_t rid);

    /// pmax tick: returns every observer whose `pmax` has elapsed since
    /// `lastSentAt`. Caller fetches current value and ships a notify.
    std::vector<ObserverContext*> due_at(std::chrono::steady_clock::time_point now);

    std::size_t size() const { return m_byKey.size(); }

private:
    std::unordered_map<Key, ObserverContext> m_byKey;

    static Key make_key(const std::string& peer, const std::string& token);
};

} // namespace lwm2m

#endif /*__lwm2m_observe_hpp__*/
