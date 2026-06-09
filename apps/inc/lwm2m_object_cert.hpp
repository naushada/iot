#ifndef __lwm2m_object_cert_hpp__
#define __lwm2m_object_cert_hpp__

#include <cstdint>
#include <functional>
#include <string>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_object_cert.hpp
 * @brief Custom LwM2M Credential-Provisioning Object (OID 2048).
 *
 * A vendor object in the reusable private range (2048–32768) that lets the
 * cloud push an end-entity TLS credential family (the OpenVPN client cert,
 * its private key, and the CA chain) to a registered device over the
 * standard LwM2M Device-Management plane — no bespoke side-channel. It is
 * the device-side half of the "iot-cloudd mints + ds-delivers" model: the
 * DM server WRITEs the artifacts, then EXECUTEs the apply trigger; the
 * device materialises the files where openvpn-client reads them and a cert
 * sidecar reloads the tunnel.
 *
 * Layout (multiple-instance — one instance per artifact):
 *
 *   Instance 0  Type = "ca"     (the CA chain)
 *   Instance 1  Type = "cert"   (the client certificate)
 *   Instance 2  Type = "key"    (the client private key)
 *
 *   RID 0  Certificate Type   String  R    "ca" | "cert" | "key"
 *   RID 1  Certificate Data   Opaque  W    raw PEM/DER bytes (staged)
 *   RID 2  Hash               String  W    lowercase hex SHA-256 of RID 1
 *   RID 3  Apply              ----    E    commit staged artifacts + reload
 *                                          (present on instance 0 only)
 *
 * Write semantics: WRITEs to RID 1/2 stage the bytes in memory. EXECUTE on
 * /2048/0/3 atomically commits every staged artifact via store_artifact()
 * then calls apply(). Staging-then-commit avoids a half-written cert family
 * if the push is interrupted between WRITEs.
 *
 * Note: this carries the private key down the wire (under DTLS). A
 * device-generated keypair + CSR-up enrolment is the stronger model and a
 * documented follow-up; the object shape is unchanged (key instance becomes
 * read-only/absent).
 */

namespace lwm2m { namespace objects {

constexpr std::uint32_t kCertObjectOid = 2048;

/// Injection points so the object stays pure + unit-testable; the client
/// wiring supplies the real filesystem + reload behaviour, tests supply
/// fakes. Unset hooks fall back to filesystem defaults under `certDir`.
struct CertHooks {
    /// Persist one committed artifact. `type` ∈ {"ca","cert","key"}; `pem`
    /// is the raw bytes from RID 1. Returns 0 on success, non-zero on
    /// failure (propagated to the EXECUTE response as 5.00). Default: write
    /// <certDir>/{ca.crt,client.crt,client.key}, key file mode 0600.
    std::function<int(const std::string& type, const std::string& pem)> store_artifact;

    /// Commit/reload trigger, called once after all artifacts are stored.
    /// Default: no-op (a cert sidecar watching certDir detects the change
    /// and reloads). Wire a ds gate-flip of services.openvpn.client.enable
    /// here for an immediate reload without a sidecar. Returns 0 on success.
    std::function<int()> apply;

    /// Optional integrity check, called per artifact at commit if a hash
    /// was written to RID 2. Returns 0 if the hash matches `pem`, non-zero
    /// to abort the apply. Default: unset → hashes are accepted as-is
    /// (stored, not enforced).
    std::function<int(const std::string& type,
                      const std::string& hexSha256,
                      const std::string& pem)> verify;
};

/// Install OID 2048 (instances 0/1/2) into `store`. `certDir` is the
/// directory the default store_artifact writes the cert family into
/// (e.g. "/etc/iot/vpn"); ignored when a store_artifact hook is supplied.
/// Returns 0 on success.
int install_cert(ObjectStore& store,
                 const std::string& certDir,
                 CertHooks hooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_cert_hpp__*/
