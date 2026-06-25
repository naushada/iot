#include "http_client.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_sys_stat.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Process.h>

#include <openssl/evp.h>

namespace containers {

namespace {

// Resolve the curl binary once. Try the usual locations, then fall back to a
// bare name (ACE_Process → execvp searches PATH). Empty only if nothing found.
std::string curl_path() {
    for (const char* p : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl"}) {
        if (ACE_OS::access(p, X_OK) == 0) return p;
    }
    return "curl";
}

// Create a temp file; returns its path ("" on failure). The file is created
// empty and closed — curl (re)writes it.
std::string make_temp(const char* tag) {
    std::string tmpl = std::string("/tmp/iot-ctr-") + tag + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) return {};
    ::close(fd);
    return std::string(buf.data());
}

std::string read_file(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// The final HTTP status across all redirect hops (-D dumps a status line per
// hop; the last one is authoritative).
long status_from_headers(const std::string& headers) {
    long last = 0;
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("HTTP/", 0) == 0) {
            auto sp = line.find(' ');
            if (sp != std::string::npos) {
                try { last = std::stol(line.substr(sp + 1)); } catch (...) {}
            }
        }
    }
    return last;
}

} // namespace

bool http_get(const std::string&              url,
              const std::vector<std::string>& accept,
              const std::string&              bearer,
              const std::string&              basic,
              const std::string&              body_path,
              bool                            follow_redirects,
              int                             timeout_sec,
              HttpResponse&                   resp,
              std::string&                    err) {
    resp = HttpResponse{};

    const std::string hdr_path = make_temp("hdr");
    const std::string err_path = make_temp("err");
    const std::string cfg_path = make_temp("cfg");
    if (hdr_path.empty() || err_path.empty() || cfg_path.empty()) {
        err = "could not create temp files";
        if (!hdr_path.empty()) ::unlink(hdr_path.c_str());
        if (!err_path.empty()) ::unlink(err_path.c_str());
        if (!cfg_path.empty()) ::unlink(cfg_path.c_str());
        return false;
    }

    // Pass EVERY option via a curl config file (-K), not argv tokens. ACE's
    // command_line(argv[]) round-trips the argv through a single space-joined
    // buffer and re-splits it on whitespace, which shattered the multi-word
    // header values ("Accept: …", "Authorization: Bearer …") — curl then saw a
    // bare -H ("option -H: requires parameter"). A config file keeps those
    // values quoted and off the argv entirely; the only argv tokens are the
    // space-free `curl -K <cfg>`. (mkstemp gives 0600, so the bearer token in
    // the file is not world-readable; we unlink it right after the wait.)
    // No -f: a 401/404 must still return so the caller can react (bearer auth /
    // not-found) rather than curl exiting with an error.
    auto q = [](const std::string& s) {            // curl-config quoted value
        std::string out = "\"";
        for (char c : s) { if (c == '\\' || c == '"') out.push_back('\\'); out.push_back(c); }
        out.push_back('"');
        return out;
    };
    {
        std::ostringstream cfg;
        cfg << "silent\nshow-error\n"
            << "connect-timeout = 20\n"
            << "max-time = " << timeout_sec << "\n"
            << "dump-header = " << q(hdr_path) << "\n"
            << "output = " << q(body_path.empty() ? std::string("/dev/null") : body_path) << "\n";
        if (follow_redirects) cfg << "location\n";
        for (const auto& a : accept) cfg << "header = " << q("Accept: " + a) << "\n";
        if (!bearer.empty()) cfg << "header = " << q("Authorization: Bearer " + bearer) << "\n";
        if (!basic.empty())  cfg << "user = " << q(basic) << "\n";
        cfg << "url = " << q(url) << "\n";
        const std::string text = cfg.str();
        FILE* cf = std::fopen(cfg_path.c_str(), "wb");
        if (!cf || std::fwrite(text.data(), 1, text.size(), cf) != text.size()) {
            if (cf) std::fclose(cf);
            err = "could not write curl config";
            ::unlink(hdr_path.c_str()); ::unlink(err_path.c_str()); ::unlink(cfg_path.c_str());
            return false;
        }
        std::fclose(cf);
    }

    std::vector<std::string> args = { curl_path(), "-K", cfg_path };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    ACE_Process_Options opts;
    opts.command_line(argv.data());

    // Redirect the child's std handles: stdin/stdout → /dev/null (curl writes
    // body to -o and headers to -D), stderr → err_path for diagnostics.
    int devnull = ::open("/dev/null", O_RDWR);
    int errfd   = ::open(err_path.c_str(), O_WRONLY | O_TRUNC);
    if (devnull >= 0 && errfd >= 0)
        opts.set_handles(devnull, devnull, errfd);

    ACE_Process proc;
    pid_t pid = proc.spawn(opts);
    if (devnull >= 0) ::close(devnull);
    if (errfd   >= 0) ::close(errfd);

    if (pid == ACE_INVALID_PID) {
        err = "failed to spawn curl";
        ::unlink(hdr_path.c_str());
        ::unlink(err_path.c_str());
        ::unlink(cfg_path.c_str());
        return false;
    }

    ACE_exitcode status = 0;
    proc.wait(&status);
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    resp.headers = read_file(hdr_path);
    resp.status  = status_from_headers(resp.headers);
    const std::string curl_err = read_file(err_path);
    ::unlink(hdr_path.c_str());
    ::unlink(err_path.c_str());
    ::unlink(cfg_path.c_str());   // holds the bearer token — remove promptly

    // Transport failure: curl exited non-zero AND we never saw an HTTP status.
    if (exit_code != 0 && resp.status == 0) {
        err = "curl exit " + std::to_string(exit_code);
        if (!curl_err.empty()) err += ": " + curl_err;
        return false;
    }
    resp.transport_ok = true;
    return true;
}

bool sha256_file(const std::string& path, std::string& hex_out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { std::fclose(f); return false; }

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    unsigned char buf[65536];
    size_t n;
    while (ok && (n = std::fread(buf, 1, sizeof buf, f)) > 0)
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;
    if (std::ferror(f)) ok = false;

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdlen = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, md, &mdlen) == 1;
    EVP_MD_CTX_free(ctx);
    std::fclose(f);
    if (!ok) return false;

    static const char* hex = "0123456789abcdef";
    hex_out.clear();
    hex_out.reserve(mdlen * 2);
    for (unsigned i = 0; i < mdlen; ++i) {
        hex_out.push_back(hex[md[i] >> 4]);
        hex_out.push_back(hex[md[i] & 0x0F]);
    }
    return true;
}

bool mkdir_p(const std::string& path) {
    std::string acc;
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t slash = path.find('/', start);
        std::string comp = (slash == std::string::npos)
                               ? path.substr(start)
                               : path.substr(start, slash - start);
        if (!comp.empty()) {
            acc += "/" + comp;       // paths are absolute (leading '/')
            if (ACE_OS::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

} // namespace containers
