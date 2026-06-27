#ifndef __lwm2m_registration_client_hpp__
#define __lwm2m_registration_client_hpp__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
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
    /// Lost-ack recovery. The FSM leaves an Awaiting*Ack state only on a
    /// response, so without a timeout a single dropped datagram strands it
    /// forever — the "registered on-device, expired at the cloud" failure.
    /// After ackTimeoutSeconds with no response we retransmit a lost Update
    /// up to maxAckRetransmits times; if that budget is spent (or a
    /// Register/Deregister ack is lost) the session is suspect and the client
    /// reconnects from scratch. Keep maxAckRetransmits * ackTimeoutSeconds <
    /// updateMarginSeconds so a transient loss is recovered before the
    /// server's lifetime expires.
    std::uint32_t  ackTimeoutSeconds{6};
    std::uint32_t  maxAckRetransmits{3};
    /// NAT keepalive cadence. The registration Update doubles as the keepalive,
    /// but it only fires at `lifetime − updateMarginSeconds` (e.g. 60 s with a
    /// 90 s lifetime) — longer than a typical NAT/CGNAT UDP idle timeout (~30 s),
    /// so the mapping dies between Updates and the source port rebinds, breaking
    /// the DTLS session. A small CoAP ping (empty CON) every keepaliveSeconds
    /// holds the mapping without perturbing the registration lifetime. 0
    /// disables it. Keep it below the link's NAT timeout (20 s is safe for a
    /// ~30 s timeout; lower for aggressive CGNAT/cellular).
    std::uint32_t  keepaliveSeconds{20};
};

class RegistrationClient {
public:
    RegistrationClient(ClientConfig cfg, const ObjectStore& store);

    /// CoAP ping — an empty Confirmable (RFC 7252 §4.3): ver=1, type=CON,
    /// TKL=0, code=0.00, the given message-id; 4 bytes. The DM answers RST.
    /// Sent on the DTLS socket between Updates as a NAT keepalive. Static (no
    /// FSM state): the reply is correlated by nothing and simply dropped.
    static std::string build_keepalive_ping(std::uint16_t messageId);

    /// Configured NAT keepalive cadence in seconds (0 = disabled).
    std::uint32_t keepalive_seconds() const { return m_cfg.keepaliveSeconds; }

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

    /// What check_ack_timeout() wants the caller to do about a lost ack.
    enum class AckRecovery : std::uint8_t {
        None,             ///< not awaiting, or still within the ack window
        RetransmitUpdate, ///< lost Update ack, within budget — caller resends
                          ///< the Update (build_update_request re-arms the FSM)
        ReRegister,       ///< budget spent, or a lost Register/Deregister ack:
                          ///< FSM is now Unregistered — caller re-establishes
                          ///< the session (replay bootstrap) and re-Registers
    };

    /// Reactor-driven; call once per tick. Detects a registration request
    /// whose response never arrived (ackTimeoutSeconds elapsed in an
    /// Awaiting*Ack state) and drives recovery so a dropped datagram can't
    /// strand the FSM forever. See ClientConfig::ackTimeoutSeconds.
    AckRecovery check_ack_timeout(std::chrono::steady_clock::time_point now);

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

    /// Update the registration endpoint live. Threadsafe (mutex).
    /// Sets the `pending_reregister` flag so the reactor tick knows
    /// to drop the current registration and rejoin under the new
    /// identity (Deregister → Unregistered → Register). For state
    /// machines NOT currently Registered the flag is harmless —
    /// the next Register the reactor sends already uses the new
    /// endpoint via `endpoint()`.
    void set_endpoint(std::string ep);
    std::string endpoint() const;

    /// Reactor tick consults these to decide whether to fire a
    /// Deregister + auto re-Register cycle after a live endpoint
    /// change. peek + clear are split so the tick can leave the
    /// flag set when state is mid-flight (AwaitingRegisterAck etc.)
    /// and pick it up on a later tick.
    bool pending_reregister() const {
        return m_re_register_pending.load(std::memory_order_relaxed);
    }
    void clear_pending_reregister() {
        m_re_register_pending.store(false, std::memory_order_relaxed);
    }

    /// Public trigger for callers that need to force a Deregister →
    /// Register cycle without changing the endpoint — typical use is
    /// a transport-layer rebind (different DM server URI). The reactor
    /// tick reacts to this flag the same way as for an endpoint change.
    void request_reregister() {
        m_re_register_pending.store(true, std::memory_order_relaxed);
    }

    /// L16/D5b — disable gate. When set true:
    ///   - the reactor tick's Unregistered → Register branch SKIPS,
    ///     so a Deregister leaves the FSM parked instead of
    ///     auto-rejoining
    ///   - request_reregister is auto-set so the next tick sends
    ///     Deregister if state is Registered
    /// When set false (re-enable):
    ///   - the reactor tick fires the Unregistered → Register path
    ///     on its next pass
    void set_disabled(bool v) {
        if (v) {
            m_re_register_pending.store(true, std::memory_order_relaxed);
        }
        m_disabled.store(v, std::memory_order_relaxed);
    }
    bool is_disabled() const {
        return m_disabled.load(std::memory_order_relaxed);
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
    /// Live mirror of m_cfg.endpoint. Same reason as m_lifetime: a
    /// data-store listener-thread set_endpoint() must not race with
    /// reactor-thread build_register_request(). Guarded by m_endpoint_mtx
    /// because std::string isn't lock-free.
    mutable std::mutex      m_endpoint_mtx;
    std::string             m_endpoint;
    std::atomic<bool>       m_re_register_pending{false};
    /// Consecutive lost-Update-ack retransmits (check_ack_timeout); reset on
    /// any received ack or on escalation to a full re-Register. Reactor-only
    /// (tick + on_response), so a plain int needs no atomic.
    std::uint32_t           m_ackRetransmits{0};
    /// L16/D5b — operator-flipped service gate. When true, the
    /// reactor tick skips auto-Register on Unregistered.
    std::atomic<bool>       m_disabled{false};
    const ObjectStore&      m_store;
    RegistrationState       m_state{RegistrationState::Unregistered};
    std::string             m_location;
    std::chrono::steady_clock::time_point m_lastSendOrAck;
};

} // namespace lwm2m

#endif /*__lwm2m_registration_client_hpp__*/
