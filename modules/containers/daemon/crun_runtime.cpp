#include "crun_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Process.h>

#include <nlohmann/json.hpp>

namespace containers {

namespace {

std::string resolve_crun() {
    for (const char* p : {"/usr/bin/crun", "/bin/crun", "/usr/local/bin/crun"}) {
        if (ACE_OS::access(p, X_OK) == 0) return p;
    }
    return "crun";
}

std::string make_temp(const char* tag) {
    std::string tmpl = std::string("/tmp/iot-crun-") + tag + "-XXXXXX";
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
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    std::fclose(f);
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\n' || s[b-1] == '\r' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

} // namespace

CrunRuntime::CrunRuntime(std::string state_root)
    : m_state_root(std::move(state_root)), m_crun(resolve_crun()) {}

int CrunRuntime::run(const std::vector<std::string>& args, std::string* out,
                     std::string& err) {
    const std::string out_path = make_temp("out");
    const std::string err_path = make_temp("err");
    if (out_path.empty() || err_path.empty()) {
        err = "could not create temp files";
        if (!out_path.empty()) ::unlink(out_path.c_str());
        if (!err_path.empty()) ::unlink(err_path.c_str());
        return -1;
    }

    // cgroupfs manager: we run as root, so crun manages the container cgroup
    // directly (no dbus / transient-scope dependency the systemd manager needs).
    std::vector<std::string> full = {m_crun, "--root", m_state_root,
                                     "--cgroup-manager", "cgroupfs"};
    full.insert(full.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(full.size() + 1);
    for (auto& s : full) argv.push_back(s.data());
    argv.push_back(nullptr);

    ACE_Process_Options opts;
    opts.command_line(argv.data());

    int devnull = ::open("/dev/null", O_RDONLY);
    int outfd   = ::open(out_path.c_str(), O_WRONLY | O_TRUNC);
    int errfd   = ::open(err_path.c_str(), O_WRONLY | O_TRUNC);
    if (devnull >= 0 && outfd >= 0 && errfd >= 0)
        opts.set_handles(devnull, outfd, errfd);

    ACE_Process proc;
    pid_t pid = proc.spawn(opts);
    if (devnull >= 0) ::close(devnull);
    if (outfd   >= 0) ::close(outfd);
    if (errfd   >= 0) ::close(errfd);

    if (pid == ACE_INVALID_PID) {
        err = "failed to spawn crun";
        ::unlink(out_path.c_str());
        ::unlink(err_path.c_str());
        return -1;
    }

    ACE_exitcode status = 0;
    proc.wait(&status);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (out) *out = read_file(out_path);
    if (code != 0) err = trim(read_file(err_path));
    ::unlink(out_path.c_str());
    ::unlink(err_path.c_str());
    return code;
}

bool CrunRuntime::create(const std::string& id, const std::string& bundle_dir,
                         std::string& err) {
    return run({"create", "--bundle", bundle_dir, id}, nullptr, err) == 0;
}

bool CrunRuntime::start(const std::string& id, std::string& err) {
    return run({"start", id}, nullptr, err) == 0;
}

CrunStatus CrunRuntime::state(const std::string& id) {
    CrunStatus st;
    std::string out, err;
    if (run({"state", id}, &out, err) != 0) return st;   // exists=false
    try {
        auto j = nlohmann::json::parse(out);
        st.exists = true;
        if (j.contains("status") && j["status"].is_string())
            st.status = j["status"].get<std::string>();
        if (j.contains("pid") && j["pid"].is_number_integer())
            st.pid = j["pid"].get<long>();
    } catch (...) {
        st.exists = false;
    }
    return st;
}

bool CrunRuntime::kill(const std::string& id, const std::string& signal,
                       std::string& err) {
    return run({"kill", id, signal}, nullptr, err) == 0;
}

bool CrunRuntime::remove(const std::string& id, bool force, std::string& err) {
    std::vector<std::string> a = {"delete"};
    if (force) a.push_back("--force");
    a.push_back(id);
    return run(a, nullptr, err) == 0;
}

} // namespace containers
