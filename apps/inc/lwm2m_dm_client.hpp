#ifndef __lwm2m_dm_client_hpp__
#define __lwm2m_dm_client_hpp__

#include <cstdint>
#include <memory>
#include <string>

#include "coap_adapter.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_observe.hpp"

/**
 * @file lwm2m_dm_client.hpp
 * @brief Device Management & Service Enablement — client-side handler.
 *
 * L5 deliverable per design §3.1 / RDD REQ-DM-001/003/004/005/006/007.
 *
 * Routes inbound DM CoAP messages on `/{oid}[/{iid}[/{rid}[/{riid}]]]` to
 * the `Resource` callbacks installed in the `ObjectStore`. The codec
 * dispatch is content-format driven:
 *   - CF=0   plain text  → lwm2m::plaintext         (REQ-ENC-005)
 *   - CF=42  opaque      → lwm2m::opaque            (REQ-ENC-006)
 *   - CF=11542 TLV       → lwm2m::tlv (via codec registry)
 *   - CF=40  link-format → lwm2m::linkformat (Discover only)
 *
 * Discover variant of GET (Accept: application/link-format) is satisfied
 * via the L2 `linkformat::discover(store, …)` helper.
 *
 * Write-Attributes is detected by a PUT with query keys `pmin` / `pmax` /
 * `gt` / `lt` / `st` and an empty payload; per D2 the attribute row
 * matching the calling server's Short Server ID is updated.
 */

namespace lwm2m {

struct DmOutcome {
    enum Kind : std::uint8_t {
        None,           ///< not a DM request (e.g. /bs, /rd, custom URI)
        Read,
        Discover,
        Write,
        Create,
        Delete,
        Execute,
        WriteAttributes,
        Observe,            ///< L7: Observe added or refreshed
        ObserveCancel,      ///< L7: Observe cancelled (Observe: 1 or RST)
        Error,              ///< 4.xx / 5.xx response built
    };

    Kind        kind{None};
    std::string response;        ///< CoAP bytes to ship (2.05 initial value on Observe)
    /// L7: zero or more Notify frames produced as a side effect of this
    /// request (e.g. a Write that triggered observers on the touched
    /// resource). Shipped by the caller after `response`.
    std::vector<std::string> notifies;
};

/// Inbound DM dispatcher. Pure logic, no socket / ACE coupling.
class DmClient {
public:
    explicit DmClient(std::shared_ptr<ObjectStore> store);

    /// v1 single-server: every request is treated as coming from this
    /// Short Server ID for the purposes of Write-Attributes keying (D2).
    /// L3+L4 set this from the registered Server Object instance the
    /// peer maps to; defaults to 1 if never set.
    void calling_short_server_id(std::uint16_t ssid) { m_callerSsid = ssid; }
    std::uint16_t calling_short_server_id() const { return m_callerSsid; }

    /// L7: identify the peer that sent the next message so the Observer
    /// can be keyed by (peer, token). DmClient itself doesn't speak ACE;
    /// the wrapping ServiceContext sets this from the incoming datagram
    /// addr before calling `handle()`.
    void calling_peer(std::string peer) { m_callerPeer = std::move(peer); }
    const std::string& calling_peer() const { return m_callerPeer; }

    /// L7 outbound trigger: invoke after a `Resource::write` happens
    /// outside the request path (e.g. background sensor poll) to fire
    /// notifies on observers of `(oid, iid, rid)`. Frames are returned
    /// for the caller to ship.
    std::vector<std::string>
    on_resource_changed(std::uint32_t oid,
                        std::uint32_t iid,
                        std::uint32_t rid,
                        const std::string& newValue);

    /// L7: drive pmax expiry. Caller invokes once per reactor tick.
    std::vector<std::string>
    tick(std::chrono::steady_clock::time_point now);

    /// L7: RST handling. Cancels every observer matching `peer`.
    std::size_t on_rst_from(const std::string& peer);

    /// Inspect a parsed CoAP message. `kind == None` means the URI does
    /// NOT match a DM target — the caller's `processRequest()` should
    /// fall through to its other handlers.
    DmOutcome handle(const CoAPAdapter::CoAPMessage& msg, CoAPAdapter& coap);

    std::shared_ptr<ObjectStore>& store() { return m_store; }
    ObserverRegistry& observers() { return m_observers; }

private:
    DmOutcome handle_observe_register(const CoAPAdapter::CoAPMessage& msg,
                                      CoAPAdapter& coap);
    DmOutcome handle_observe_cancel(const CoAPAdapter::CoAPMessage& msg);

    std::shared_ptr<ObjectStore>  m_store;
    std::uint16_t                 m_callerSsid{1};
    std::string                   m_callerPeer;
    ObserverRegistry              m_observers;

    /// L7: monotonic message-id allocator for engine-emitted notify frames.
    std::uint16_t                 m_notifyMsgId{0x8000};
    std::uint16_t                 next_notify_msgid() { return ++m_notifyMsgId; }
};

} // namespace lwm2m

#endif /*__lwm2m_dm_client_hpp__*/
