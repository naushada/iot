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
    std::string cmd = m_openssl + " " + args + " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D cloudd:thread:%t %M %N:%l openssl failed rc=%d: %C\n"),
                   rc, args.c_str()));
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

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D cloudd:thread:%t %M %N:%l runtime CA + server cert "
                        "generated (ca=%C server=%C)\n"),
               m_p.ca_crt.c_str(), m_p.srv_crt.c_str()));
    return true;
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

    bool ok =
        run_openssl("genrsa -out '" + key + "' 2048") &&
        run_openssl("req -new -key '" + key + "' -out '" + csr +
                    "' -subj '/O=IoT Cloud/CN=" + safe + "'") &&
        run_openssl("x509 -req -days " + std::to_string(m_p.days) + " -in '" + csr +
                    "' -CA '" + m_p.ca_crt + "' -CAkey '" + m_p.ca_key +
                    "' -CAcreateserial -out '" + crt + "'");

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
