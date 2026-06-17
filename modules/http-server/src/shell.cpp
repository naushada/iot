#include "shell.hpp"

#include <ace/Log_Msg.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// forkpty(3) lives in <pty.h> on glibc, <util.h> on macOS/BSD.
#if defined(__APPLE__) || defined(__FreeBSD__)
#  include <util.h>
#else
#  include <pty.h>
#endif

namespace http_server {

namespace {

// One PTY read chunk + the per-poll cap. The kernel PTY buffer is ~64 KiB;
// we drain up to kMaxReadPerPoll into one response so a flood (e.g. `yes`)
// can't grow the JSON body without bound — the rest is read on the next poll.
constexpr std::size_t kReadChunk      = 16 * 1024;
constexpr std::size_t kMaxReadPerPoll = 256 * 1024;

const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

// ─────────────────────────── base64 ────────────────────────────
std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) |
                          (static_cast<unsigned char>(in[i + 1]) << 8) |
                          (static_cast<unsigned char>(in[i + 2]));
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back(kB64[(n >> 6) & 0x3F]);
        out.push_back(kB64[n & 0x3F]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        std::uint32_t n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) |
                          (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back(kB64[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(const std::string& in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;  // skip '=', whitespace, anything else
    };
    std::string out;
    out.reserve((in.size() / 4) * 3);
    int  acc = 0, bits = 0;
    for (unsigned char c : in) {
        int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ─────────────────────────── ShellSession ──────────────────────
ShellSession::ShellSession(int master_fd, pid_t pid)
    : m_master_fd(master_fd), m_pid(pid),
      m_last_active(std::chrono::steady_clock::now()) {}

ShellSession::~ShellSession() {
    // Closing the master sends SIGHUP to the child's foreground group;
    // SIGKILL is the backstop for a shell that ignores it, and waitpid
    // reaps the zombie.
    if (m_master_fd >= 0) ::close(m_master_fd);
    if (m_pid > 0) {
        ::kill(m_pid, SIGKILL);
        int st = 0;
        ::waitpid(m_pid, &st, 0);
    }
}

void ShellSession::touch() {
    std::lock_guard<std::mutex> lk(m_meta_mtx);
    m_last_active = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point ShellSession::last_active() const {
    std::lock_guard<std::mutex> lk(m_meta_mtx);
    return m_last_active;
}

std::string ShellSession::read_output(int timeout_sec, bool& closed) {
    std::lock_guard<std::mutex> lk(m_read_mtx);
    touch();
    closed = false;

    struct pollfd pfd;
    pfd.fd      = m_master_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int pr = ::poll(&pfd, 1, timeout_sec * 1000);
    if (pr <= 0) return {};  // timeout or EINTR — no data this round

    std::string out;
    if (pfd.revents & POLLIN) {
        char buf[kReadChunk];
        while (out.size() < kMaxReadPerPoll) {
            ssize_t n = ::read(m_master_fd, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));
                if (static_cast<std::size_t>(n) < sizeof(buf)) break;
                continue;  // drained this read; loop for more
            }
            if (n == 0) { closed = true; break; }  // EOF
            // On Linux a read of a master whose slave has closed returns
            // EIO — that is the child exiting, not a real error.
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            closed = true;  // EIO / fatal
            break;
        }
    }
    if (!closed && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) &&
        out.empty()) {
        closed = true;
    }
    touch();
    return out;
}

bool ShellSession::write_input(const std::string& data) {
    touch();
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(m_master_fd, data.data() + off, data.size() - off);
        if (n > 0) { off += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Non-blocking master, slave not draining fast enough. Brief
            // wait for writability rather than busy-spinning.
            struct pollfd pfd;
            pfd.fd = m_master_fd; pfd.events = POLLOUT; pfd.revents = 0;
            if (::poll(&pfd, 1, 1000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

void ShellSession::resize(unsigned short cols, unsigned short rows) {
    touch();
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols;
    ws.ws_row = rows;
    ::ioctl(m_master_fd, TIOCSWINSZ, &ws);
}

void ShellSession::signal_hangup() {
    if (m_pid > 0) ::kill(m_pid, SIGHUP);
}

// ─────────────────────────── ShellManager ──────────────────────
ShellManager::ShellManager(std::size_t max_sessions, int idle_sec)
    : m_max_sessions(max_sessions ? max_sessions : 1),
      m_idle_sec(idle_sec > 0 ? idle_sec : 300) {
    m_reaper = std::thread([this] { reaper_loop(); });
}

ShellManager::~ShellManager() {
    {
        std::lock_guard<std::mutex> lk(m_reaper_mtx);
        m_stop = true;
    }
    m_reaper_cv.notify_all();
    if (m_reaper.joinable()) m_reaper.join();
    // Sessions teardown via ~ShellSession (SIGHUP/KILL + reap) as the map
    // clears here.
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.clear();
}

std::string ShellManager::make_sid() {
    // 128-bit random hex from /dev/urandom; unique enough for a session id.
    unsigned char raw[16];
    std::memset(raw, 0, sizeof(raw));
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t got = 0;
        while (got < sizeof(raw)) {
            ssize_t n = ::read(fd, raw + got, sizeof(raw) - got);
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
    static const char* hex = "0123456789abcdef";
    std::string sid;
    sid.reserve(sizeof(raw) * 2);
    for (unsigned char b : raw) {
        sid.push_back(hex[b >> 4]);
        sid.push_back(hex[b & 0x0F]);
    }
    return sid;
}

std::string ShellManager::open(unsigned short cols, unsigned short rows,
                               std::string& err) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_sessions.size() >= m_max_sessions) {
            err = "too many shell sessions";
            return {};
        }
    }
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols ? cols : 80;
    ws.ws_row = rows ? rows : 24;

    int   master = -1;
    pid_t pid    = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        err = std::string("forkpty: ") + std::strerror(errno);
        return {};
    }
    if (pid == 0) {
        // ── Child ──────────────────────────────────────────────
        ::setenv("TERM", "xterm-256color", 1);
        const char* shell = ::getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        // Interactive login-ish shell. argv[0] starting with '-' would make
        // it a login shell; "-i" keeps it interactive (prompt, job control)
        // which is what the browser terminal wants.
        ::execl(shell, shell, "-i", static_cast<char*>(nullptr));
        ::_exit(127);  // exec failed → EOF on master, caller sees closed
    }

    // ── Parent ─────────────────────────────────────────────────
    // Non-blocking master: read_output() polls for readiness, so a read
    // after a spurious wake never blocks, and write_input() can detect a
    // backed-up slave instead of stalling the worker thread.
    int fl = ::fcntl(master, F_GETFL, 0);
    if (fl >= 0) ::fcntl(master, F_SETFL, fl | O_NONBLOCK);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);

    auto sess = std::make_shared<ShellSession>(master, pid);
    std::string sid = make_sid();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_sessions.emplace(sid, sess);
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D httpd:thread:%t %M %N:%l shell open sid=%C pid=%d "
                        "%dx%d\n"),
               sid.c_str(), static_cast<int>(pid), cols, rows));
    return sid;
}

std::shared_ptr<ShellSession> ShellManager::find(const std::string& sid) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_sessions.find(sid);
    return it == m_sessions.end() ? nullptr : it->second;
}

void ShellManager::close(const std::string& sid) {
    std::shared_ptr<ShellSession> sess;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_sessions.find(sid);
        if (it == m_sessions.end()) return;
        sess = it->second;
        m_sessions.erase(it);
    }
    // SIGHUP wakes any in-flight /output poll (POLLHUP) so it returns
    // closed promptly; the object is destroyed when the last ref drops.
    if (sess) sess->signal_hangup();
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D httpd:thread:%t %M %N:%l shell close sid=%C\n"),
               sid.c_str()));
}

void ShellManager::set_idle_sec(int s) {
    if (s > 0) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_idle_sec = s;
    }
}

void ShellManager::set_max_sessions(std::size_t n) {
    if (n > 0) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_max_sessions = n;
    }
}

void ShellManager::reaper_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_reaper_mtx);
            m_reaper_cv.wait_for(lk, std::chrono::seconds(30),
                                 [this] { return m_stop; });
            if (m_stop) return;
        }
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> dead;
        int idle_sec;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            idle_sec = m_idle_sec;
            for (auto& [sid, sess] : m_sessions) {
                if (now - sess->last_active() >
                    std::chrono::seconds(idle_sec)) {
                    dead.push_back(sid);
                }
            }
        }
        for (const auto& sid : dead) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D httpd:thread:%t %M %N:%l shell reap idle "
                                "sid=%C (>%ds)\n"),
                       sid.c_str(), idle_sec));
            close(sid);
        }
    }
}

} // namespace http_server
