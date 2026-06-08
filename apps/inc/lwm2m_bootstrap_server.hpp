#ifndef __lwm2m_bootstrap_server_hpp__
#define __lwm2m_bootstrap_server_hpp__

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "coap_adapter.hpp"
#include "lwm2m_bootstrap.hpp"

/**
 * @file lwm2m_bootstrap_server.hpp
 * @brief LwM2M Bootstrap interface — server side.
 *
 * L4 deliverable per design §5.2 / RDD REQ-BS-006..009.
 *
 * On `POST /bs?ep=…`:
 *   1. Look up the provisioned AccountProvisioning for the endpoint.
 *      Unknown endpoints get a 4.04 Not Found.
 *   2. Reply 2.04 Changed (the ACK to the client's request).
 *   3. Emit one `PUT /0/{iid}` per SecurityInstance, encoded as a single
 *      TLV container.
 *   4. Emit one `PUT /1/{iid}` per ServerInstance.
 *   5. Emit a final `POST /bs` (Bootstrap-Finish).
 *
 * v1 emits the PUTs/POST as Non-confirmable to keep the FSM simple — no
 * retransmit / ACK queue. This is allowed by Core §6.1.5 (NON allowed for
 * the Bootstrap-Write set). L4 cleanup can promote to CON + retransmit.
 *
 * Pure logic. The caller (CoAPAdapter) ships each frame in order via
 * `UDPAdapter::tx`.
 */

namespace lwm2m { namespace bootstrap {

class Server {
public:
    /// Result of one /bs request: ordered frames to send back to the
    /// peer (ACK first, then PUTs, then Finish).
    struct Result {
        bool                       handled{false};
        std::vector<std::string>   frames;
        std::string                endpoint;  ///< echoed for logging / D3 hook
    };

    Server() = default;

    /// Resolve a provisioning record for an endpoint at /bs time.
    ///
    /// The cloud Bootstrap server uses this to synthesise the DM account
    /// live from `cloud.endpoint.credentials` (the per-endpoint DM PSK that
    /// iot-cloudd minted at provisioning time and that lwm2m-dm already
    /// trusts) instead of from a static config file. Returning nullopt
    /// falls back to the statically provisioned `add_account()` map, so the
    /// device-side / test path is unchanged.
    using ProvisioningResolver =
        std::function<std::optional<AccountProvisioning>(
            const std::string& endpoint)>;
    void provisioning_resolver(ProvisioningResolver r) {
        m_resolver = std::move(r);
    }

    /// Register one provisioning record.
    void add_account(AccountProvisioning a);

    /// Look up a provisioning record by endpoint, nullptr if absent.
    const AccountProvisioning* find(const std::string& endpoint) const;

    /// Inspect a parsed CoAP message. Returns handled=false for non-/bs
    /// messages so the caller can pass them to the next handler.
    Result handle(const CoAPAdapter::CoAPMessage& msg, CoAPAdapter& coap);

private:
    std::unordered_map<std::string, AccountProvisioning> m_accounts;
    ProvisioningResolver                                 m_resolver;
};

}} // namespace lwm2m::bootstrap

#endif /*__lwm2m_bootstrap_server_hpp__*/
