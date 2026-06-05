#ifndef __http_server_tls_hpp__
#define __http_server_tls_hpp__

/// TLS termination for iot-httpd (L18 — native HTTPS).
///
/// Mirrors the xpmile inner-TLS idiom: an OpenSSL SSL object driven through
/// a pair of memory BIOs. The session pumps ciphertext between the BIOs and
/// the existing ACE_SOCK_Stream, so OpenSSL never blocks on the socket and
/// the whole thing stays on the single reactor thread (no handshake stall).
///
///   recv() ciphertext ─▶ feed_ciphertext() ─▶ [rbio] ─▶ SSL ─▶ read_plaintext()
///   send() ciphertext ◀─ drain_outgoing()  ◀─ [wbio] ◀─ SSL ◀─ write_plaintext()

#include <cstddef>
#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace http_server {

namespace detail {
struct SslCtxDeleter { void operator()(SSL_CTX* p) const noexcept { SSL_CTX_free(p); } };
struct SslDeleter    { void operator()(SSL* p)     const noexcept { SSL_free(p); } };
} // namespace detail

using SslCtxPtr = std::unique_ptr<SSL_CTX, detail::SslCtxDeleter>;
using SslPtr    = std::unique_ptr<SSL,     detail::SslDeleter>;

/// Process-wide server TLS context (one SSL_CTX shared by every connection).
/// Holds the certificate chain + private key; optionally a CA bundle that,
/// when present, switches on mutual-TLS (client certs required + verified).
class TlsContext {
public:
    TlsContext() = default;

    /// Load a server context. `cert_path` is a PEM chain (leaf first),
    /// `key_path` its private key. A non-empty `ca_path` enables mTLS.
    /// Returns true on success; on failure see err().
    bool load_server(const std::string& cert_path,
                     const std::string& key_path,
                     const std::string& ca_path = "");

    SSL_CTX*            native() const { return m_ctx.get(); }
    bool                mtls() const { return m_mtls; }
    const std::string&  err() const { return m_err; }
    explicit operator bool() const { return m_ctx != nullptr; }

private:
    SslCtxPtr   m_ctx;
    bool        m_mtls = false;
    std::string m_err;
};

/// Per-connection TLS engine. Construct from a loaded TlsContext, then drive
/// it from the reactor: feed received ciphertext, run handshake() until done,
/// then read_plaintext()/write_plaintext(), always draining outgoing
/// ciphertext to the socket after each step.
class TlsConn {
public:
    explicit TlsConn(const TlsContext& ctx);

    bool valid() const { return m_ssl != nullptr; }

    /// Hand received ciphertext (off the socket) to the TLS engine.
    void feed_ciphertext(const char* data, std::size_t len);

    /// Advance the handshake. Returns 1 = complete, 0 = needs more I/O
    /// (stay registered), -1 = fatal error.
    int  handshake();
    bool handshake_done() const { return m_done; }

    /// Append any decrypted application data to `out`. Returns 0 on success
    /// (out may stay empty if no full record yet), -1 on fatal error/close.
    int  read_plaintext(std::string& out);

    /// Encrypt `len` bytes of application data (queued into the write BIO).
    /// Returns 0 on success, -1 on error. Call drain_outgoing() afterwards.
    int  write_plaintext(const char* data, std::size_t len);

    /// Move pending outbound ciphertext out of the write BIO into `out`
    /// (to be sent on the socket). Returns the number of bytes appended.
    std::size_t drain_outgoing(std::string& out);

    /// Best-effort TLS close-notify (queued into the write BIO).
    void shutdown();

private:
    SslPtr m_ssl;
    BIO*   m_rbio = nullptr;   // owned by m_ssl after SSL_set_bio
    BIO*   m_wbio = nullptr;   // owned by m_ssl after SSL_set_bio
    bool   m_done = false;
};

} // namespace http_server

#endif
