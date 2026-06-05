#include "tls.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>

namespace http_server {

namespace {

std::string ssl_err() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown (empty error queue)";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

} // namespace

// ─────────────────────────────── TlsContext ───────────────────────────────

bool TlsContext::load_server(const std::string& cert_path,
                             const std::string& key_path,
                             const std::string& ca_path) {
    m_ctx.reset(SSL_CTX_new(TLS_server_method()));
    if (!m_ctx) {
        m_err = "SSL_CTX_new: " + ssl_err();
        return false;
    }

    // Floor at TLS 1.2 — no SSLv3/TLS1.0/1.1.
    SSL_CTX_set_min_proto_version(m_ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_options(m_ctx.get(), SSL_OP_NO_RENEGOTIATION);

    // Leaf-first PEM chain so intermediates are served too.
    if (SSL_CTX_use_certificate_chain_file(m_ctx.get(), cert_path.c_str()) != 1) {
        m_err = "certificate " + cert_path + ": " + ssl_err();
        m_ctx.reset();
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(m_ctx.get(), key_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        m_err = "private key " + key_path + ": " + ssl_err();
        m_ctx.reset();
        return false;
    }
    if (SSL_CTX_check_private_key(m_ctx.get()) != 1) {
        m_err = "private key does not match certificate: " + ssl_err();
        m_ctx.reset();
        return false;
    }

    // Optional mutual-TLS: verify client certs against the supplied CA.
    if (!ca_path.empty()) {
        if (SSL_CTX_load_verify_locations(m_ctx.get(), ca_path.c_str(),
                                          nullptr) != 1) {
            m_err = "CA bundle " + ca_path + ": " + ssl_err();
            m_ctx.reset();
            return false;
        }
        SSL_CTX_set_verify(m_ctx.get(),
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           nullptr);
        m_mtls = true;
    }

    m_err.clear();
    return true;
}

// ──────────────────────────────── TlsConn ─────────────────────────────────

TlsConn::TlsConn(const TlsContext& ctx) {
    if (!ctx) return;
    m_ssl.reset(SSL_new(ctx.native()));
    if (!m_ssl) return;

    // Memory BIOs: the session shuttles bytes between these and the socket.
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    if (!m_rbio || !m_wbio) {
        // SSL_set_bio takes ownership only on success; free explicitly here.
        if (m_rbio) BIO_free(m_rbio);
        if (m_wbio) BIO_free(m_wbio);
        m_rbio = m_wbio = nullptr;
        m_ssl.reset();
        return;
    }
    SSL_set_bio(m_ssl.get(), m_rbio, m_wbio);  // m_ssl now owns both BIOs
    SSL_set_accept_state(m_ssl.get());
}

void TlsConn::feed_ciphertext(const char* data, std::size_t len) {
    if (m_ssl && len) {
        BIO_write(m_rbio, data, static_cast<int>(len));
    }
}

int TlsConn::handshake() {
    if (!m_ssl) return -1;

    int ret = SSL_accept(m_ssl.get());
    if (ret == 1) {
        m_done = true;
        return 1;
    }
    int e = SSL_get_error(m_ssl.get(), ret);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        return 0;  // need to exchange more handshake bytes
    }
    return -1;     // fatal (bad cert, protocol error, client-cert failure)
}

int TlsConn::read_plaintext(std::string& out) {
    if (!m_ssl) return -1;

    char buf[4096];
    // One peer SSL_write may span several TLS records; loop until WANT_READ
    // so we hand the parser the whole request body in one go.
    for (;;) {
        int n = SSL_read(m_ssl.get(), buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        int e = SSL_get_error(m_ssl.get(), n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            return 0;  // drained what's buffered; wait for more ciphertext
        }
        if (e == SSL_ERROR_ZERO_RETURN) {
            return -1; // peer sent close_notify
        }
        return -1;     // fatal
    }
}

int TlsConn::write_plaintext(const char* data, std::size_t len) {
    if (!m_ssl) return -1;
    if (len == 0) return 0;
    int n = SSL_write(m_ssl.get(), data, static_cast<int>(len));
    if (n <= 0) {
        int e = SSL_get_error(m_ssl.get(), n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
        return -1;
    }
    return 0;
}

std::size_t TlsConn::drain_outgoing(std::string& out) {
    if (!m_ssl) return 0;
    std::size_t total = 0;
    char buf[4096];
    for (int pending = BIO_pending(m_wbio); pending > 0;
         pending = BIO_pending(m_wbio)) {
        int n = BIO_read(m_wbio, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        total += static_cast<std::size_t>(n);
    }
    return total;
}

void TlsConn::shutdown() {
    if (m_ssl) SSL_shutdown(m_ssl.get());
}

} // namespace http_server
