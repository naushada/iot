/// TLS tests for iot-httpd — drive a real OpenSSL client through our
/// TlsConn server over in-memory ciphertext shuttling. No ACE, no sockets:
/// pure OpenSSL, so this runs anywhere libssl is present.

#include "tls.hpp"

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <string>

#include <unistd.h>

namespace {

// ── Throwaway certificate generation (in-process, no openssl CLI) ──────────

EVP_PKEY* gen_key() { return EVP_RSA_gen(2048); }

// X509 for `cn` with `subjectKey`, signed by `issuerKey` under `issuerCn`
// (== cn for self-signed). `ca` stamps the CA:TRUE basic constraint so the
// cert can act as a trust anchor. Caller owns the returned X509.
X509* make_cert(EVP_PKEY* subjectKey, const std::string& cn,
                EVP_PKEY* issuerKey, const std::string& issuerCn,
                long serial, bool ca) {
    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60L * 24L * 365L);
    X509_set_pubkey(x, subjectKey);

    auto set_cn = [](X509_NAME* nm, const std::string& cn) {
        X509_NAME_add_entry_by_txt(
            nm, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    };
    set_cn(X509_get_subject_name(x), cn);

    X509_NAME* iname = X509_NAME_new();
    set_cn(iname, issuerCn);
    X509_set_issuer_name(x, iname);
    X509_NAME_free(iname);

    if (ca) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
        if (X509_EXTENSION* e = X509V3_EXT_conf_nid(
                nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE")) {
            X509_add_ext(x, e, -1);
            X509_EXTENSION_free(e);
        }
    }

    X509_sign(x, issuerKey, EVP_sha256());
    return x;
}

bool write_x509(const std::string& path, X509* x) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    int r = PEM_write_X509(f, x);
    std::fclose(f);
    return r == 1;
}

bool write_key(const std::string& path, EVP_PKEY* k) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    int r = PEM_write_PrivateKey(f, k, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(f);
    return r == 1;
}

std::string drain_bio(BIO* b) {
    std::string out;
    char buf[4096];
    while (BIO_pending(b) > 0) {
        int n = BIO_read(b, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

// Shuttle handshake ciphertext between an OpenSSL client and our server.
// Returns 1 = both completed, -1 = server rejected the client, 0 = stalled.
int drive_handshake(http_server::TlsConn& server, SSL* cssl) {
    for (int i = 0; i < 40; ++i) {
        SSL_do_handshake(cssl);
        std::string c2s = drain_bio(SSL_get_wbio(cssl));
        if (!c2s.empty()) server.feed_ciphertext(c2s.data(), c2s.size());

        int hs = server.handshake();
        std::string s2c;
        server.drain_outgoing(s2c);
        if (!s2c.empty())
            BIO_write(SSL_get_rbio(cssl), s2c.data(),
                      static_cast<int>(s2c.size()));

        if (hs < 0) return -1;
        if (server.handshake_done() && SSL_is_init_finished(cssl)) return 1;
    }
    return 0;
}

std::string g_tmpdir;  // per-process temp dir for PEM files
std::string tmp(const std::string& name) { return g_tmpdir + "/" + name; }

} // namespace

using http_server::TlsConn;
using http_server::TlsContext;

class Tls : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        char tmpl[] = "/tmp/iot_tls_XXXXXX";
        char* d = mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        g_tmpdir = d;

        // A CA, a server cert, and a client cert — all signed by the CA.
        EVP_PKEY* caKey  = gen_key();
        EVP_PKEY* srvKey = gen_key();
        EVP_PKEY* cliKey = gen_key();
        X509* caCert  = make_cert(caKey,  "Test CA",   caKey, "Test CA", 1, true);
        X509* srvCert = make_cert(srvKey, "localhost", caKey, "Test CA", 2, false);
        X509* cliCert = make_cert(cliKey, "iot-client", caKey, "Test CA", 3, false);

        ASSERT_TRUE(write_x509(tmp("ca.pem"),  caCert));
        ASSERT_TRUE(write_x509(tmp("srv.pem"), srvCert));
        ASSERT_TRUE(write_key (tmp("srv.key"), srvKey));
        ASSERT_TRUE(write_x509(tmp("cli.pem"), cliCert));
        ASSERT_TRUE(write_key (tmp("cli.key"), cliKey));

        X509_free(caCert); X509_free(srvCert); X509_free(cliCert);
        EVP_PKEY_free(caKey); EVP_PKEY_free(srvKey); EVP_PKEY_free(cliKey);
    }

    // A fresh OpenSSL client over memory BIOs. `with_cert` presents the
    // client cert (mTLS); `max12` pins TLS 1.2 so client auth is verified
    // during the handshake (deterministic for the reject test).
    static SSL* make_client(SSL_CTX** out_ctx, bool with_cert, bool max12) {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);  // self-signed CA
        if (max12) SSL_CTX_set_max_proto_version(c, TLS1_2_VERSION);
        if (with_cert) {
            SSL_CTX_use_certificate_file(c, tmp("cli.pem").c_str(),
                                         SSL_FILETYPE_PEM);
            SSL_CTX_use_PrivateKey_file(c, tmp("cli.key").c_str(),
                                        SSL_FILETYPE_PEM);
        }
        SSL* s = SSL_new(c);
        SSL_set_bio(s, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
        SSL_set_connect_state(s);
        *out_ctx = c;
        return s;
    }
};

TEST_F(Tls, LoadServerBadCertFails) {
    TlsContext ctx;
    EXPECT_FALSE(ctx.load_server("/no/such/cert.pem", "/no/such/key.pem"));
    EXPECT_FALSE(ctx.err().empty());
    EXPECT_FALSE(static_cast<bool>(ctx));
}

TEST_F(Tls, LoadServerGood) {
    TlsContext ctx;
    EXPECT_TRUE(ctx.load_server(tmp("srv.pem"), tmp("srv.key"))) << ctx.err();
    EXPECT_TRUE(static_cast<bool>(ctx));
    EXPECT_FALSE(ctx.mtls());  // no CA → server-only TLS
}

TEST_F(Tls, HandshakeAndEcho) {
    TlsContext ctx;
    ASSERT_TRUE(ctx.load_server(tmp("srv.pem"), tmp("srv.key"))) << ctx.err();

    TlsConn server(ctx);
    ASSERT_TRUE(server.valid());

    SSL_CTX* cctx = nullptr;
    SSL* cssl = make_client(&cctx, /*with_cert*/ false, /*max12*/ false);
    ASSERT_EQ(drive_handshake(server, cssl), 1);

    // Client → server request.
    const std::string req =
        "POST /api/v1/db/get HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
    ASSERT_GT(SSL_write(cssl, req.data(), static_cast<int>(req.size())), 0);
    std::string c2s = drain_bio(SSL_get_wbio(cssl));
    server.feed_ciphertext(c2s.data(), c2s.size());
    std::string got;
    ASSERT_EQ(server.read_plaintext(got), 0);
    EXPECT_EQ(got, req);

    // Server → client response.
    const std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(server.write_plaintext(resp.data(), resp.size()), 0);
    std::string s2c;
    server.drain_outgoing(s2c);
    BIO_write(SSL_get_rbio(cssl), s2c.data(), static_cast<int>(s2c.size()));
    char rbuf[512];
    int rn = SSL_read(cssl, rbuf, sizeof(rbuf));
    ASSERT_GT(rn, 0);
    EXPECT_EQ(std::string(rbuf, static_cast<std::size_t>(rn)), resp);

    SSL_free(cssl);
    SSL_CTX_free(cctx);
}

// mTLS enabled: a client presenting a CA-signed cert is accepted.
TEST_F(Tls, MutualTlsClientWithCertAccepted) {
    TlsContext ctx;
    ASSERT_TRUE(ctx.load_server(tmp("srv.pem"), tmp("srv.key"), tmp("ca.pem")))
        << ctx.err();
    EXPECT_TRUE(ctx.mtls());

    TlsConn server(ctx);
    SSL_CTX* cctx = nullptr;
    SSL* cssl = make_client(&cctx, /*with_cert*/ true, /*max12*/ true);
    EXPECT_EQ(drive_handshake(server, cssl), 1);  // handshake completes

    SSL_free(cssl);
    SSL_CTX_free(cctx);
}

// mTLS enabled: a client with NO certificate is rejected at handshake.
TEST_F(Tls, MutualTlsClientWithoutCertRejected) {
    TlsContext ctx;
    ASSERT_TRUE(ctx.load_server(tmp("srv.pem"), tmp("srv.key"), tmp("ca.pem")))
        << ctx.err();
    EXPECT_TRUE(ctx.mtls());

    TlsConn server(ctx);
    SSL_CTX* cctx = nullptr;
    SSL* cssl = make_client(&cctx, /*with_cert*/ false, /*max12*/ true);
    EXPECT_EQ(drive_handshake(server, cssl), -1);  // server rejects, no cert

    SSL_free(cssl);
    SSL_CTX_free(cctx);
}
