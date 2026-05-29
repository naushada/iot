#ifndef __lwm2m_bootstrap_client_hpp__
#define __lwm2m_bootstrap_client_hpp__

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coap_adapter.hpp"
#include "dtls_adapter.hpp"
#include "lwm2m_bootstrap.hpp"
#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_bootstrap_client.hpp
 * @brief LwM2M Bootstrap interface — client side.
 *
 * L4 deliverable per design §5.1 / RDD REQ-BS-001..005.
 *
 * Pure logic; the caller sends bytes via `UDPAdapter::tx` and feeds
 * incoming bytes via `parseRequest`.
 *
 * FSM (per RDD §3.1):
 *   Idle
 *     │  client started + BS configured
 *     ▼
 *   SendBSRequest    → build_register_request bytes
 *     │  2.04 Changed ACK
 *     ▼
 *   WaitForBSWrites  ← PUT /0/{iid}, PUT /1/{iid}, DELETE /, DELETE /{oid}/{iid}
 *     │  POST /bs (Bootstrap-Finish)
 *     ▼
 *   ApplyBootstrap   → atomic commit (REQ-BS-004): all staged writes copied
 *     │                 into the live ObjectStore + DTLSAdapter PSK store
 *     ▼
 *   Done             → caller's on_done_cb() fires; typical caller starts
 *                      the L3 RegistrationClient against the freshly-
 *                      installed Server Object.
 */

namespace lwm2m { namespace bootstrap {

enum class ClientState : std::uint8_t {
    Idle,
    AwaitingBSAck,
    WaitForBSWrites,
    Done,
    Failed,
};

/// Staged mutations between BS-ACK and Bootstrap-Finish. Applied atomically
/// on Finish so a half-completed BS exchange leaves the live store intact
/// (REQ-BS-004).
struct StagingBuffer {
    bool                            purge{false};       ///< DELETE / received
    std::vector<std::uint32_t>      deletedObjectInstances; ///< encoded as oid*256 + iid
    std::vector<SecurityInstance>   security;
    std::vector<ServerInstance>     server;

    void clear() { *this = StagingBuffer{}; }
};

class Client {
public:
    using DoneCb = std::function<void(const StagingBuffer& committed)>;

    Client(std::string endpoint,
           std::shared_ptr<ObjectStore> liveStore,
           std::shared_ptr<DTLSAdapter> dtls);

    /* ────────── client-driven traffic ──────────────────────────── */

    /// Build POST /bs?ep=… (REQ-BS-001).
    std::string build_bs_request(std::uint16_t messageId,
                                 const std::string& token);

    /* ────────── server-driven traffic (we are the responder) ───── */

    /// Inspect a parsed CoAP message from the Bootstrap-Server. Returns
    /// the response bytes to ship back (a 2.04 ACK on a successful staged
    /// write, 4.00 Bad Request on a malformed payload, empty when the
    /// message is unrelated).
    std::string handle_bs_traffic(const CoAPAdapter::CoAPMessage& msg,
                                  CoAPAdapter& coap);

    /// Hook fired after a successful commit. The caller typically starts
    /// a RegistrationClient against the committed Server Object.
    void on_done(DoneCb cb) { m_onDone = std::move(cb); }

    ClientState  state()    const { return m_state; }
    const StagingBuffer& staging() const { return m_staging; }

private:
    void apply_commit();
    bool decode_security_write(std::uint16_t iid,
                               const std::string& payload,
                               SecurityInstance& out);
    bool decode_server_write(std::uint16_t iid,
                             const std::string& payload,
                             ServerInstance& out);

    std::string                  m_endpoint;
    std::shared_ptr<ObjectStore> m_store;
    std::shared_ptr<DTLSAdapter> m_dtls;
    ClientState                  m_state{ClientState::Idle};
    StagingBuffer                m_staging;
    DoneCb                       m_onDone;
};

}} // namespace lwm2m::bootstrap

#endif /*__lwm2m_bootstrap_client_hpp__*/
