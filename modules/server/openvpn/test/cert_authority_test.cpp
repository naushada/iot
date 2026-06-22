#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "cert_authority.hpp"

using server::openvpn::CaPaths;
using server::openvpn::CertAuthority;

namespace {

bool have_openssl() { return std::system("/usr/bin/openssl version >/dev/null 2>&1") == 0; }

bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

std::string slurp(const std::string& p) {
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream ss; ss << ifs.rdbuf(); return ss.str();
}

// A throwaway working dir; SKIP if no writable scratch.
std::string scratch() {
    char tmpl[] = "/tmp/iot-ca-test-XXXXXX";
    char* d = ::mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

CaPaths paths_under(const std::string& dir) {
    CaPaths p;
    ::mkdir((dir + "/ca").c_str(), 0755);
    p.ca_key  = dir + "/ca/ca.key";
    p.ca_crt  = dir + "/ca/ca.crt";
    p.srv_key = dir + "/server.key";
    p.srv_crt = dir + "/server.crt";
    p.crl       = dir + "/ca/crl.pem";
    p.ca_db     = dir + "/ca/index.txt";
    p.ca_serial = dir + "/ca/serial";
    p.ca_crlnum = dir + "/ca/crlnumber";
    p.ca_cnf    = dir + "/ca/openssl.cnf";
    p.ca_newdir = dir + "/ca/newcerts";
    return p;
}

// True if `openssl verify -crl_check` ACCEPTS the cert against the CA + CRL.
bool crl_verify_ok(const std::string& ca_crt, const std::string& crl,
                   const std::string& cert) {
    return std::system(("/usr/bin/openssl verify -crl_check -CAfile '" + ca_crt +
                        "' -CRLfile '" + crl + "' '" + cert +
                        "' >/dev/null 2>&1").c_str()) == 0;
}

} // namespace

/* ─────────────────────────── sanitize_cn (pure) ───────────────────────── */

TEST(CertAuthority, sanitize_cn_keeps_safe_charset_and_maps_rest) {
    EXPECT_EQ("rpi100000abcd@cloud.local",
              CertAuthority::sanitize_cn("rpi100000abcd@cloud.local"));
    // shell metacharacters are neutralised
    EXPECT_EQ("a_b_c_rm_-rf", CertAuthority::sanitize_cn("a b/c;rm -rf"));
    EXPECT_EQ("x__y", CertAuthority::sanitize_cn("x'`y"));
    // never empty
    EXPECT_EQ("device", CertAuthority::sanitize_cn(""));
    // capped at 64
    EXPECT_LE(CertAuthority::sanitize_cn(std::string(200, 'a')).size(), 64u);
}

/* ─────────────────────── ensure + mint (needs openssl) ────────────────── */

TEST(CertAuthority, ensure_generates_ca_and_server_then_is_idempotent) {
    if (!have_openssl()) GTEST_SKIP() << "openssl CLI not available";
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);

    CertAuthority ca(p);
    EXPECT_FALSE(ca.have_ca());
    ASSERT_TRUE(ca.ensure());
    EXPECT_TRUE(ca.have_ca());
    EXPECT_TRUE(exists(p.ca_key));
    EXPECT_TRUE(exists(p.ca_crt));
    EXPECT_TRUE(exists(p.srv_crt));
    EXPECT_TRUE(exists(p.srv_key));

    // CA key + server key must be private (0600).
    struct stat st;
    ASSERT_EQ(0, ::stat(p.ca_key.c_str(), &st));  EXPECT_EQ(0600, st.st_mode & 0777);
    ASSERT_EQ(0, ::stat(p.srv_key.c_str(), &st)); EXPECT_EQ(0600, st.st_mode & 0777);

    // Idempotent: a second ensure() leaves the CA key byte-identical.
    const std::string ca_before = slurp(p.ca_key);
    ASSERT_TRUE(ca.ensure());
    EXPECT_EQ(ca_before, slurp(p.ca_key));
}

TEST(CertAuthority, mint_client_returns_family_that_chains_to_ca) {
    if (!have_openssl()) GTEST_SKIP() << "openssl CLI not available";
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);

    CertAuthority ca(p);
    ASSERT_TRUE(ca.ensure());

    auto mc = ca.mint_client("rpi100000abcd@cloud.local");
    ASSERT_TRUE(mc.has_value());
    EXPECT_NE(std::string::npos, mc->client_crt.find("BEGIN CERTIFICATE"));
    EXPECT_NE(std::string::npos, mc->client_key.find("PRIVATE KEY"));
    EXPECT_EQ(slurp(p.ca_crt), mc->ca_crt);

    // The minted client cert AND the server cert both verify against the CA.
    const std::string crt = dir + "/client.crt";
    std::ofstream(crt, std::ios::binary) << mc->client_crt;
    EXPECT_EQ(0, std::system(("/usr/bin/openssl verify -CAfile '" + p.ca_crt +
                              "' '" + crt + "' >/dev/null 2>&1").c_str()));
    EXPECT_EQ(0, std::system(("/usr/bin/openssl verify -CAfile '" + p.ca_crt +
                              "' '" + p.srv_crt + "' >/dev/null 2>&1").c_str()));
}

TEST(CertAuthority, mint_without_ca_fails) {
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);
    CertAuthority ca(p);                 // ensure() not called
    EXPECT_FALSE(ca.mint_client("x").has_value());
}

/* ─────────────────────── CRL / revocation (needs openssl) ─────────────── */

TEST(CertAuthority, ensure_stands_up_crl_database_and_initial_crl) {
    if (!have_openssl()) GTEST_SKIP() << "openssl CLI not available";
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);

    CertAuthority ca(p);
    ASSERT_TRUE(ca.ensure());
    EXPECT_TRUE(exists(p.crl));
    EXPECT_TRUE(exists(p.ca_db));
    EXPECT_TRUE(exists(p.ca_cnf));
    auto pem = ca.crl_pem();
    ASSERT_TRUE(pem.has_value());
    EXPECT_NE(std::string::npos, pem->find("BEGIN X509 CRL"));
}

TEST(CertAuthority, revoked_cert_is_rejected_while_others_still_verify) {
    if (!have_openssl()) GTEST_SKIP() << "openssl CLI not available";
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);

    CertAuthority ca(p);
    ASSERT_TRUE(ca.ensure());

    auto keep = ca.mint_client("rpi-keep@cloud.local");
    auto gone = ca.mint_client("rpi-gone@cloud.local");
    ASSERT_TRUE(keep.has_value());
    ASSERT_TRUE(gone.has_value());

    const std::string keepCrt = dir + "/keep.crt";
    const std::string goneCrt = dir + "/gone.crt";
    std::ofstream(keepCrt, std::ios::binary) << keep->client_crt;
    std::ofstream(goneCrt, std::ios::binary) << gone->client_crt;

    // Before revocation both verify against the (empty) CRL.
    EXPECT_TRUE(crl_verify_ok(p.ca_crt, p.crl, keepCrt));
    EXPECT_TRUE(crl_verify_ok(p.ca_crt, p.crl, goneCrt));

    // Revoke one → a fresh CRL is returned + persisted.
    auto crl = ca.revoke(gone->client_crt);
    ASSERT_TRUE(crl.has_value());
    EXPECT_NE(std::string::npos, crl->find("BEGIN X509 CRL"));

    // Now the revoked cert is rejected; the other still passes.
    EXPECT_FALSE(crl_verify_ok(p.ca_crt, p.crl, goneCrt));
    EXPECT_TRUE(crl_verify_ok(p.ca_crt, p.crl, keepCrt));
}

TEST(CertAuthority, revoke_foreign_cert_fails) {
    if (!have_openssl()) GTEST_SKIP() << "openssl CLI not available";
    const std::string dir = scratch();
    if (dir.empty()) GTEST_SKIP() << "no writable scratch dir";
    CaPaths p = paths_under(dir);

    CertAuthority ca(p);
    ASSERT_TRUE(ca.ensure());
    // A cert not minted by this CA's database cannot be revoked.
    EXPECT_FALSE(ca.revoke("-----BEGIN CERTIFICATE-----\nnotacert\n"
                           "-----END CERTIFICATE-----\n").has_value());
}
