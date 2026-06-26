#include "cert_authority.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <ace/Log_Msg.h>

namespace server {
namespace openvpn {

namespace {

bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string dirname_of(const std::string& p) {
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

bool slurp(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << data;
    return ofs.good();
}

} // namespace

CertAuthority::CertAuthority(CaPaths paths, std::string openssl)
    : m_p(std::move(paths)), m_openssl(std::move(openssl)) {}

std::string CertAuthority::sanitize_cn(const std::string& cn) {
    std::string out;
    out.reserve(cn.size());
    for (char c : cn) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '@' || c == '-';
        out.push_back(ok ? c : '_');
        if (out.size() >= 64) break;
    }
    if (out.empty()) out = "device";
    return out;
}

bool CertAuthority::run_openssl(const std::string& args) const {
    // Single point where we invoke the CLI. Subjects are fixed strings and
    // the only externally-influenced value (the CN) is sanitised to a shell-
    // safe charset by sanitize_cn() before it ever reaches here, then wrapped
    // in single quotes by the caller — so there is no metacharacter that can
    // escape the quoted -subj argument.
    // Capture stderr (openssl's own diagnostic — TXT_DB / serial / policy errors
    // from `openssl ca`, etc.) instead of swallowing it to /dev/null, so a mint
    // failure is actually diagnosable in the cloud log. cloudd runs the CA on a
    // single thread, so a fixed temp path in the CA dir can't race.
    const std::string errf = dirname_of(m_p.ca_key) + "/.openssl.err";
    std::string cmd = m_openssl + " " + args + " >/dev/null 2>'" + errf + "'";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::string err;
        slurp(errf, err);
        if (err.size() > 400) err = err.substr(err.size() - 400);   // bound the line
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l openssl failed rc=%d: %C\n"
                            "  openssl stderr: %C\n"),
                   rc, args.c_str(), err.c_str()));
        return false;
    }
    return true;
}

bool CertAuthority::have_ca() const {
    return file_exists(m_p.ca_key) && file_exists(m_p.ca_crt);
}

bool CertAuthority::ensure() {
    if (file_exists(m_p.ca_key)) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l CA key present (%C); "
                            "runtime CA already initialised\n"),
                   m_p.ca_key.c_str()));
        // Backfill the CRL/CA-database scaffolding for a CA that predates the
        // CRL feature. Best-effort: a CRL failure must not fail CA bring-up.
        if (!ensure_crl())
            ACE_ERROR((LM_WARNING,
                       ACE_TEXT("%D cloudd:thread:%t %M %N:%l CRL scaffolding "
                                "backfill failed (revocation unavailable)\n")));
        return true;
    }

    // Make sure the CA dir exists (the iot-vpn volume ships /etc/iot/vpn/ca
    // with ca.crt, but be defensive on a clean volume).
    ::mkdir(dirname_of(m_p.ca_key).c_str(), 0755);

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l no CA key — generating a "
                        "runtime CA + server cert (build-time CA key was purged)\n")));

    const std::string days = std::to_string(m_p.days);

    // 1) CA keypair + self-signed cert.
    if (!run_openssl("genrsa -out '" + m_p.ca_key + "' 4096")) return false;
    ::chmod(m_p.ca_key.c_str(), 0600);
    if (!run_openssl("req -new -x509 -days " + days +
                     " -key '" + m_p.ca_key + "' -out '" + m_p.ca_crt +
                     "' -subj '" + m_p.ca_subj + "'")) return false;

    // 2) Server keypair + cert signed by the new CA (replaces the build-time
    //    server cert, which was signed by the purged build CA).
    const std::string csr = dirname_of(m_p.srv_crt) + "/server.csr";
    const std::string srl = dirname_of(m_p.ca_crt)  + "/ca.srl";
    if (!run_openssl("genrsa -out '" + m_p.srv_key + "' 2048")) return false;
    ::chmod(m_p.srv_key.c_str(), 0600);
    if (!run_openssl("req -new -key '" + m_p.srv_key + "' -out '" + csr +
                     "' -subj '" + m_p.srv_subj + "'")) return false;
    if (!run_openssl("x509 -req -days " + days + " -in '" + csr +
                     "' -CA '" + m_p.ca_crt + "' -CAkey '" + m_p.ca_key +
                     "' -CAcreateserial -out '" + m_p.srv_crt + "'")) return false;
    ::unlink(csr.c_str());
    (void)srl;

    // Stand up the CRL/CA-database scaffolding so per-device certs minted below
    // are revocable. Best-effort: a CRL failure must not fail CA bring-up.
    if (!ensure_crl())
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l CRL scaffolding init "
                            "failed (revocation unavailable)\n")));

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l runtime CA + server cert "
                        "generated (ca=%C server=%C)\n"),
               m_p.ca_crt.c_str(), m_p.srv_crt.c_str()));
    return true;
}

bool CertAuthority::write_ca_cnf() const {
    std::ostringstream c;
    c << "[ ca ]\n"
      << "default_ca = CA_default\n\n"
      << "[ CA_default ]\n"
      << "dir              = " << dirname_of(m_p.ca_crt) << "\n"
      << "database         = " << m_p.ca_db     << "\n"
      << "new_certs_dir    = " << m_p.ca_newdir << "\n"
      << "serial           = " << m_p.ca_serial << "\n"
      << "crlnumber        = " << m_p.ca_crlnum << "\n"
      << "certificate      = " << m_p.ca_crt    << "\n"
      << "private_key      = " << m_p.ca_key    << "\n"
      << "default_md       = sha256\n"
      << "default_days     = " << m_p.days      << "\n"
      << "default_crl_days = " << m_p.crl_days  << "\n"
      << "policy           = policy_any\n"
      << "email_in_dn      = no\n"
      << "unique_subject   = no\n"
      << "copy_extensions  = none\n"
      << "preserve         = no\n\n"
      << "[ policy_any ]\n"
      << "commonName             = supplied\n"
      << "organizationName       = optional\n"
      << "organizationalUnitName = optional\n"
      << "countryName            = optional\n"
      << "stateOrProvinceName    = optional\n"
      << "localityName           = optional\n"
      << "emailAddress           = optional\n";
    return write_file(m_p.ca_cnf, c.str());
}

bool CertAuthority::ensure_crl() {
    if (!have_ca()) return false;
    ::mkdir(m_p.ca_newdir.c_str(), 0755);
    if (!file_exists(m_p.ca_db)     && !write_file(m_p.ca_db, ""))         return false;
    // openssl reads unique_subject from index.txt.attr if present; force "no"
    // so re-provisioning the same CN (a re-mint) doesn't error on a duplicate.
    write_file(m_p.ca_db + ".attr", "unique_subject = no\n");
    if (!file_exists(m_p.ca_serial) && !write_file(m_p.ca_serial, "01\n")) return false;
    if (!file_exists(m_p.ca_crlnum) && !write_file(m_p.ca_crlnum, "01\n")) return false;
    if (!write_ca_cnf()) return false;
    // Emit an initial (empty) CRL so the server always has a crl-verify target.
    if (!file_exists(m_p.crl)) {
        if (!run_openssl("ca -batch -config '" + m_p.ca_cnf +
                         "' -gencrl -out '" + m_p.crl + "'")) return false;
    }
    return true;
}

std::optional<std::string> CertAuthority::crl_pem() const {
    std::string pem;
    if (!slurp(m_p.crl, pem)) return std::nullopt;
    return pem;
}

std::optional<std::string> CertAuthority::revoke(const std::string& client_cert_pem) {
    if (!have_ca() || !ensure_crl()) return std::nullopt;

    char tmpl[] = "/tmp/iot-revoke-XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l revoke: mkdtemp failed "
                            "errno=%d\n"), errno));
        return std::nullopt;
    }
    const std::string d    = dir;
    const std::string cert = d + "/cert.pem";

    std::optional<std::string> result;
    if (write_file(cert, client_cert_pem) &&
        run_openssl("ca -batch -config '" + m_p.ca_cnf + "' -revoke '" + cert + "'") &&
        run_openssl("ca -batch -config '" + m_p.ca_cnf +
                    "' -gencrl -out '" + m_p.crl + "'")) {
        std::string pem;
        if (slurp(m_p.crl, pem)) result = std::move(pem);
    }
    ::unlink(cert.c_str());
    ::rmdir(d.c_str());

    if (!result)
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l revoke failed (cert not "
                            "minted by this CA's database, or openssl error)\n")));
    return result;
}

std::optional<MintedCert> CertAuthority::mint_client(const std::string& cn) {
    if (!have_ca()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l mint_client(%C): no CA "
                            "available (call ensure() first)\n"),
                   cn.c_str()));
        return std::nullopt;
    }
    const std::string safe = sanitize_cn(cn);

    // Work in a private temp dir so concurrent/repeated mints don't collide
    // and nothing leaks into the persistent volume.
    char tmpl[] = "/tmp/iot-mint-XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l mint_client: mkdtemp "
                            "failed errno=%d\n"), errno));
        return std::nullopt;
    }
    const std::string d   = dir;
    const std::string key = d + "/client.key";
    const std::string csr = d + "/client.csr";
    const std::string crt = d + "/client.crt";

    // Sign through `openssl ca` (not `x509 -req`) so the cert is recorded in the
    // CA database (index.txt) and can be revoked later. `-notext` keeps the
    // output a clean PEM (no human-readable header). ensure_crl() first so the
    // database + openssl.cnf exist.
    bool ok =
        ensure_crl() &&
        run_openssl("genrsa -out '" + key + "' 2048") &&
        run_openssl("req -new -key '" + key + "' -out '" + csr +
                    "' -subj '/O=IoT Cloud/CN=" + safe + "'") &&
        run_openssl("ca -batch -notext -config '" + m_p.ca_cnf + "' -days " +
                    std::to_string(m_p.days) + " -in '" + csr + "' -out '" + crt + "'");

    MintedCert out;
    if (ok) {
        ok = slurp(crt, out.client_crt) &&
             slurp(key, out.client_key) &&
             slurp(m_p.ca_crt, out.ca_crt);
    }

    // Best-effort cleanup of the temp material.
    ::unlink(key.c_str()); ::unlink(csr.c_str()); ::unlink(crt.c_str());
    ::rmdir(d.c_str());

    if (!ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l mint_client(%C) failed\n"),
                   safe.c_str()));
        return std::nullopt;
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l minted client cert CN=%C "
                        "(%d bytes)\n"),
               safe.c_str(), static_cast<int>(out.client_crt.size())));
    return out;
}

} // namespace openvpn
} // namespace server
