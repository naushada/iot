#ifndef __lwm2m_registration_client_hpp__
#define __lwm2m_registration_client_hpp__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "coap_adapter.hpp"
#include "lwm2m_codec_linkformat.hpp"
#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_registration_client.hpp
 * @brief LwM2M Registration interface — client side FSM.
 *
 * L3 deliverable per design §5.3 / RDD REQ-REG-001 / 003 / 004 / 006 /
 * 007 / 011.
 *
 * Pure logic: builds CoAP message bytes and consumes server responses;
 * does NOT touch sockets, ACE, or DTLS. Send is the caller's job — they
 * hand the byte string to `UDPAdapter::tx(payload, service)` which the ACE
 * refactor made thread-safe.
 *
 * D2 keying: a future multi-server build instantiates one
 * `RegistrationClient` per Short Server ID. v1 instantiates one.
 */

namespace lwm2m {

/// What the FSM is currently doing.
enum class RegistrationState : std::uint8_t {
    Unregistered,        ///< no Register sent yet, or last Deregister succeeded
    AwaitingRegisterAck, ///< Register sent; waiting for 2.01 Created + Location
    Registered,          ///< 2.01 received; lifetime ticker running
    AwaitingUpdateAck,   ///< Update sent; waiting for 2.04 Changed
    AwaitingDeregisterAck,
    Failed,              ///< 4.xx / 5.xx received or repeated timeout
};

/// Identity + lifetime + advertised set + Short Server ID. Constructed at
/// the end of Bootstrap (L4) and passed in to RegistrationClient.
struct ClientConfig {
    std::uint16_t  shortServerId{1};         ///< D2
    std::string    endpoint;                 ///< "ep" query
    std::uint32_t  lifetime{86400};          ///< "lt" query
    std::string    binding{"U"};             ///< "b" query
    std::string    smsNumber;                ///< "sms" query (optional)
    std::string    lwm2mVersion{"1.1"};      ///< "lwm2m" query — pinned by D1
    /// 30 s default per RDD REQ-REG-006 minimum margin.
    std::uint32_t  updateMarginSeconds{30};
};

class RegistrationClient {
public:
    RegistrationClient(ClientConfig cfg, const ObjectStore& store);

    /// Build POST /rd request bytes per REQ-REG-001 / 007. Caller supplies
    /// a fresh message ID and 0..8 token bytes; we echo them in the
    /// response handlers below.
    std::string build_register_request(std::uint16_t messageId,
                                       const std::string& token);

    /// Build POST /rd/{loc} update bytes. `withAdvertisedSet=false` skips
    /// the link-format payload (lifetime-only refresh — REQ-REG-003).
    std::string build_update_request(std::uint16_t messageId,
                                     const std::string& token,
                                     bool withAdvertisedSet);

    /// Build DELETE /rd/{loc} bytes per REQ-REG-004.
    std::string build_deregister_request(std::uint16_t messageId,
                                         const std::string& token);

    /// Consume a parsed CoAP response. The state machine advances based on
    /// the message code (2.01 → Registered, 2.04 → still Registered,
    /// 2.02 → Unregistered, 4.xx/5.xx → Failed).
    void on_response(const CoAPAdapter::CoAPMessage& msg,
                     CoAPAdapter& coapHelper);

    /// Reactor-driven 1 Hz tick. Returns true if it is time to send an
    /// Update — caller then invokes build_update_request and ships it.
    /// REQ-REG-006: triggers at `now + margin >= expiresAt`.
    bool should_send_update(std::chrono::steady_clock::time_point now) const;

    /// Record that an Update was sent at `t`; resets the next-expected
    /// expiry to `t + lifetime`.
    void note_update_sent(std::chrono::steady_clock::time_point t =
                              std::chrono::steady_clock::now());

    /// Update the registration lifetime live. The next Register or
    /// Update built will carry the new `lt=` query value; the
    /// `should_send_update` window shrinks/grows immediately because
    /// the next tick reads the atomic. Thread-safe — the typical
    /// caller is the data-store listener thread reacting to a NotifyEvent.
    void set_lifetime(std::uint32_t seconds) {
        m_lifetime.store(seconds, std::memory_order_relaxed);
    }
    std::uint32_t lifetime() const {
        return m_lifetime.load(std::memory_order_relaxed);
    }

    RegistrationState   state()    const { return m_state; }
    const std::string&  location() const { return m_location; }
    const ClientConfig& config()   const { return m_cfg; }

private:
    ClientConfig            m_cfg;
    /// Live mirror of m_cfg.lifetime. m_cfg is no longer the source
    /// of truth for lifetime — readers consult this atomic so a
    /// concurrent set_lifetime() takes effect on the next request
    /// build / tick without locking the reactor.
    std::atomic<std::uint32_t> m_lifetime;
    const ObjectStore&      m_store;
    RegistrationState       m_state{RegistrationState::Unregistered};
    std::string             m_location;
    std::chrono::steady_clock::time_point m_lastSendOrAck;
};

} // namespace lwm2m

#endif /*__lwm2m_registration_client_hpp__*/
