#ifndef __server_openvpn_cert_authority_hpp__
#define __server_openvpn_cert_authority_hpp__

/// Runtime VPN PKI for iot-cloudd (Phase 2 of the cert-provisioning work).
///
/// The cloud image generates a CA + server cert at BUILD time but PURGES the
/// CA private key, so the shipped image can verify clients but cannot sign
/// new ones. This component moves the CA to RUNTIME: on first boot it
/// generates a CA keypair (persisted in the iot-vpn volume) and regenerates
/// the openvpn server cert signed by it, so the server trusts every client
/// cert this CA later mints. Per-device client certs are minted at
/// provision time and delivered to the device over LwM2M Object 2048.
///
/// Implementation shells out to the `openssl(1)` CLI (present in the cloud
/// runtime image) — the same commands the Dockerfile uses to bootstrap the
/// build-time PKI, so the trust chain is identical.

#include <optional>
#include <string>

namespace server {
namespace openvpn {

/// Filesystem locations for the CA + server material. Defaults match the
/// cloud image layout (and cloud.vpn.* ds defaults). ca_key lives next to
/// ca_crt under the persistent iot-vpn volume so it survives restarts.
struct CaPaths {
    std::string ca_key    = "/etc/iot/vpn/ca/ca.key";
    std::string ca_crt    = "/etc/iot/vpn/ca/ca.crt";
    std::string srv_key   = "/etc/iot/vpn/server.key";
    std::string srv_crt   = "/etc/iot/vpn/server.crt";
    std::string ca_subj   = "/O=IoT Cloud/CN=iot-cloud-ca";
    std::string srv_subj  = "/O=IoT Cloud/CN=cloud-vpn";
    int         days      = 3650;
    // CRL / CA-database material (Phase 2 of apps/docs/tdd-device-transfer.md).
    // Minting goes through `openssl ca` (vs `x509 -req`) so issued certs land in
    // `ca_db` (index.txt) and can later be revoked + rolled into a CRL.
    std::string crl       = "/etc/iot/vpn/ca/crl.pem";
    std::string ca_db     = "/etc/iot/vpn/ca/index.txt";
    std::string ca_serial = "/etc/iot/vpn/ca/serial";
    std::string ca_crlnum = "/etc/iot/vpn/ca/crlnumber";
    std::string ca_cnf    = "/etc/iot/vpn/ca/openssl.cnf";
    std::string ca_newdir = "/etc/iot/vpn/ca/newcerts";
    int         crl_days  = 3650;
};

/// A freshly minted client credential family. ca_crt is included so the
/// device gets the full trust set in one push.
struct MintedCert {
    std::string client_crt;   ///< PEM
    std::string client_key;   ///< PEM
    std::string ca_crt;       ///< PEM (the signing CA)
};

class CertAuthority {
public:
    explicit CertAuthority(CaPaths paths = {},
                           std::string openssl = "/usr/bin/openssl");

    /// Ensure a usable CA exists. If `ca_key` is absent, generate a fresh CA
    /// AND regenerate the server cert/key signed by it (so the openvpn server
    /// trusts certs this CA mints). Idempotent: if `ca_key` already exists,
    /// returns true without touching anything. MUST be called before the
    /// openvpn server starts. Returns false on any openssl/IO failure.
    bool ensure();

    /// True once a CA private key is present (after a successful ensure()).
    bool have_ca() const;

    /// Mint a client cert + key signed by the CA, CN = sanitized `cn`. Signed
    /// via `openssl ca` so the cert is recorded in the CA database and is
    /// revocable later. Returns std::nullopt on failure (no CA, openssl/IO).
    std::optional<MintedCert> mint_client(const std::string& cn);

    /// Ensure the CRL/CA-database scaffolding exists (openssl.cnf, index.txt,
    /// serial, crlnumber, newcerts/, and an initial empty CRL). Idempotent and
    /// safe to call on an already-initialised CA (e.g. a cloud upgraded into
    /// the CRL feature). Called from ensure() and before mint/revoke. Returns
    /// false if there is no CA or on openssl/IO failure.
    bool ensure_crl();

    /// Revoke a previously-minted client cert (passed as its PEM) and roll a
    /// fresh CRL. Returns the new CRL PEM on success. Fails (nullopt) if the
    /// cert was not minted by this CA's database (e.g. issued before the CRL
    /// feature) — only `openssl ca`-minted certs are revocable.
    std::optional<std::string> revoke(const std::string& client_cert_pem);

    /// Current CRL PEM (empty optional if none yet). Read-only.
    std::optional<std::string> crl_pem() const;

    /// CN sanitiser exposed for tests: keep [A-Za-z0-9._@-], map the rest to
    /// '_', cap at 64 chars, never empty.
    static std::string sanitize_cn(const std::string& cn);

private:
    bool run_openssl(const std::string& args) const;  // args appended to the binary
    bool write_ca_cnf() const;                         // (re)write openssl.cnf

    CaPaths     m_p;
    std::string m_openssl;
};

} // namespace openvpn
} // namespace server

#endif /* __server_openvpn_cert_authority_hpp__ */
