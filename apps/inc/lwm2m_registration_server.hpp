#ifndef __lwm2m_registration_server_hpp__
#define __lwm2m_registration_server_hpp__

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "coap_adapter.hpp"
#include "lwm2m_registration.hpp"

/**
 * @file lwm2m_registration_server.hpp
 * @brief LwM2M Registration interface — server side.
 *
 * L3 deliverable. CoAP-side handler for the three Registration operations:
 *   POST   /rd?ep=…&lt=…&lwm2m=…&b=…  → 2.01 + Location-Path
 *   POST   /rd/{loc}[?lt=…&b=…]       → 2.04
 *   DELETE /rd/{loc}                  → 2.02
 *
 * Pure-logic — does NOT own a socket, does NOT call ACE. Receives a parsed
 * `CoAPMessage`, mutates the `ClientRegistry`, returns response bytes ready
 * for `CoAPAdapter::tx`.
 *
 * Closes REQ-REG-001 / REQ-REG-002 / REQ-REG-003 / REQ-REG-004 /
 * REQ-REG-005 / REQ-REG-007 / REQ-REG-009.
 */

namespace lwm2m {

/// Result of a CoAP request: serialised reply bytes and a hint about what
/// happened so the mirror / hooks can be fired by the wrapper.
struct RegistrationOutcome {
    enum Kind {
        None,           ///< not a /rd request
        Created,        ///< a new registration was added; `location` is set
        Updated,        ///< an existing registration's lifetime was refreshed
        Removed,        ///< Deregister succeeded; `location` was removed
        BadRequest,     ///< malformed query / missing ep
        NotFound,       ///< unknown location
    };

    Kind        kind{None};
    std::string response;       ///< CoAP bytes to ship (empty when kind==None)
    std::string location;       ///< populated for Created / Updated / Removed
};

/**
 * @brief Routes /rd CoAP messages to the registry.
 *
 * A single instance per Server / Bootstrap-Server. Held by the
 * `ServiceContext_t` post-L3 wiring.
 */
class RegistrationServer {
public:
    /// Callback fired on every successful Create / Update / Remove so the
    /// async Mongo mirror (lwm2m_registry_mirror.hpp) can stay in sync
    /// without ClientRegistry needing to know about it. Optional.
    using EventCb = std::function<void(const RegistrationOutcome& outcome,
                                       const ServerRegistration*  snapshot)>;

    explicit RegistrationServer(std::shared_ptr<ClientRegistry> reg);

    /// Inspect a parsed CoAP message and produce the response if it's a
    /// /rd-family request. Non-/rd messages return `kind == None` and an
    /// empty response so the caller can pass the message to the next
    /// handler.
    ///
    /// `peerHost`/`peerPort` are used to populate ServerRegistration.peer*
    /// on Register; they have no effect on Update/Deregister responses.
    RegistrationOutcome handle(const CoAPAdapter::CoAPMessage& msg,
                               CoAPAdapter& coapHelper,
                               const std::string& peerHost,
                               std::uint16_t peerPort);

    void on_event(EventCb cb) { m_event = std::move(cb); }
    std::shared_ptr<ClientRegistry> registry() { return m_registry; }

    /// L16/D5b — operator-flipped service gate. When set true,
    /// `handle()` rejects new Register requests (POST /rd) with
    /// 5.03 Service Unavailable. Update / Deregister continue to
    /// process so currently-registered clients can clean up.
    /// Active registrations are dropped at the transition by the
    /// caller (registry()->load_from({})).
    void set_disabled(bool v) {
        m_disabled.store(v, std::memory_order_relaxed);
    }
    bool is_disabled() const {
        return m_disabled.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<ClientRegistry> m_registry;
    EventCb                         m_event;
    std::atomic<bool>               m_disabled{false};
};

} // namespace lwm2m

#endif /*__lwm2m_registration_server_hpp__*/
