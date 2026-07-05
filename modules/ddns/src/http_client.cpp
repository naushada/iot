#include "ddns/http_client.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include <ace/OS_NS_unistd.h>
#include <ace/Process.h>

namespace ddns {

namespace {

// Resolve the curl binary once (ACE_Process → execvp searches PATH otherwise).
std::string curl_path() {
    for (const char* p : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl"}) {
        if (ACE_OS::access(p, X_OK) == 0) return p;
    }
    return "curl";
}

// Create a 0600 temp file; returns its path ("" on failure).
std::string make_temp(const char* tag) {
    std::string tmpl = std::string("/tmp/iot-ddns-") + tag + "-XXXXXX";
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

bool write_file(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = data.empty() ||
              std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

// The final HTTP status across all redirect hops (last HTTP/ line wins).
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

// curl-config quoted value.
std::string q(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '\\' || c == '"') out.push_back('\\'); out.push_back(c); }
    out.push_back('"');
    return out;
}

} // namespace

bool http_request(const std::string&              method,
                  const std::string&              url,
                  const std::vector<std::string>& headers,
                  const std::string&              basic,
                  const std::string&              body,
                  int                             timeout_sec,
                  HttpResponse&                   resp,
                  std::string&                    err) {
    resp = HttpResponse{};

    const std::string hdr_path  = make_temp("hdr");
    const std::string out_path  = make_temp("out");
    const std::string err_path  = make_temp("err");
    const std::string cfg_path  = make_temp("cfg");
    const std::string data_path = body.empty() ? std::string() : make_temp("data");
    auto cleanup = [&] {
        for (const auto* p : {&hdr_path, &out_path, &err_path, &cfg_path, &data_path})
            if (!p->empty()) ::unlink(p->c_str());
    };
    if (hdr_path.empty() || out_path.empty() || err_path.empty() || cfg_path.empty() ||
        (!body.empty() && data_path.empty())) {
        err = "could not create temp files";
        cleanup();
        return false;
    }

    if (!body.empty() && !write_file(data_path, body)) {
        err = "could not write request body";
        cleanup();
        return false;
    }

    // All options via a -K config file; only argv tokens are `curl -K <cfg>`.
    // The config file (mkstemp 0600) may hold the Basic user:pass + bearer
    // headers, so it is unlinked right after wait().
    {
        std::ostringstream cfg;
        cfg << "silent\nshow-error\n"
            << "connect-timeout = 20\n"
            << "max-time = " << timeout_sec << "\n"
            << "request = " << q(method) << "\n"
            << "dump-header = " << q(hdr_path) << "\n"
            << "output = " << q(out_path) << "\n"
            << "location\n";
        for (const auto& h : headers) cfg << "header = " << q(h) << "\n";
        if (!basic.empty()) cfg << "user = " << q(basic) << "\n";
        if (!body.empty())  cfg << "data-binary = " << q("@" + data_path) << "\n";
        cfg << "url = " << q(url) << "\n";
        if (!write_file(cfg_path, cfg.str())) {
            err = "could not write curl config";
            cleanup();
            return false;
        }
    }

    std::vector<std::string> args = { curl_path(), "-K", cfg_path };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    ACE_Process_Options opts;
    opts.command_line(argv.data());

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
        cleanup();
        return false;
    }

    ACE_exitcode status = 0;
    proc.wait(&status);
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    resp.headers = read_file(hdr_path);
    resp.status  = status_from_headers(resp.headers);
    resp.body    = read_file(out_path);
    const std::string curl_err = read_file(err_path);
    cleanup();

    if (exit_code != 0 && resp.status == 0) {
        err = "curl exit " + std::to_string(exit_code);
        if (!curl_err.empty()) err += ": " + curl_err;
        return false;
    }
    resp.transport_ok = true;
    return true;
}

bool http_get(const std::string& url, int timeout_sec,
              HttpResponse& resp, std::string& err) {
    return http_request("GET", url, {}, "", "", timeout_sec, resp, err);
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace ddns
