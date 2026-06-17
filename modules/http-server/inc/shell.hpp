#ifndef __http_server_shell_hpp__
#define __http_server_shell_hpp__

// Remote-shell support for the device-ui Terminal page.
//
// A ShellSession wraps a forkpty(3)-backed interactive shell: the master
// fd is the parent end of the PTY, the child execs a login shell. The
// browser drives it over three long-poll/POST endpoints (see
// handler_shell.cpp):
//   GET  /api/v1/shell/output  — long-poll PTY output (base64)
//   POST /api/v1/shell/input   — write keystrokes (base64)
//   POST /api/v1/shell/resize  — TIOCSWINSZ
// Output is drained lazily inside the /output poll (the kernel PTY buffer
// holds bytes between polls), so there is no per-session reader thread and
// nothing to register with the ACE reactor — it slots into the existing
// blocking-long-poll worker model exactly like /api/v1/db/get.
//
// This is a remote ROOT shell: every endpoint is Admin-gated AND behind
// the http.shell.enabled master switch (default off). See schemas/http.lua.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>  // pid_t

namespace data_store { class Client; }

namespace http_server {

class Router;        // fwd (router.hpp)
class SessionStore;  // fwd (auth.hpp)

class ShellSession {
public:
    ShellSession(int master_fd, pid_t pid);
    ~ShellSession();

    ShellSession(const ShellSession&)            = delete;
    ShellSession& operator=(const ShellSession&) = delete;

    /// Block up to `timeout_sec` for PTY output and return whatever bytes
    /// are available (raw; empty on a clean timeout). `closed` is set when
    /// the child has exited / the PTY hit EOF — the returned bytes may be
    /// non-empty even then (final output before exit). Serialised per
    /// session so two concurrent /output polls can't interleave reads.
    std::string read_output(int timeout_sec, bool& closed);

    /// Write keystrokes (raw bytes) to the PTY. Returns false on error.
    bool write_input(const std::string& data);

    /// Resize the PTY window (TIOCSWINSZ).
    void resize(unsigned short cols, unsigned short rows);

    /// Ask the child to exit (SIGHUP) so an in-flight read_output() wakes
    /// promptly with closed=true.
    void signal_hangup();

    std::chrono::steady_clock::time_point last_active() const;

private:
    void touch();

    int                m_master_fd;
    pid_t              m_pid;
    std::mutex         m_read_mtx;   // serialises read_output()
    mutable std::mutex m_meta_mtx;   // guards m_last_active
    std::chrono::steady_clock::time_point m_last_active;
};

class ShellManager {
public:
    ShellManager(std::size_t max_sessions, int idle_sec);
    ~ShellManager();

    ShellManager(const ShellManager&)            = delete;
    ShellManager& operator=(const ShellManager&) = delete;

    /// forkpty a new shell sized cols x rows. Returns the new opaque
    /// session id, or "" on failure (with `err` set: fork/pty error or
    /// max-sessions reached).
    std::string open(unsigned short cols, unsigned short rows,
                     std::string& err);

    /// Look up a live session (shared_ptr keeps it alive across a
    /// concurrent close() while an /output poll is mid-flight).
    std::shared_ptr<ShellSession> find(const std::string& sid);

    /// SIGHUP + reap + drop the session. No-op if unknown.
    void close(const std::string& sid);

    // Tunables refreshed from ds (http.shell.idle.sec / .max.sessions).
    void set_idle_sec(int s);
    void set_max_sessions(std::size_t n);

private:
    std::string make_sid();
    void        reaper_loop();

    std::mutex m_mtx;
    std::map<std::string, std::shared_ptr<ShellSession>> m_sessions;
    std::size_t m_max_sessions;
    int         m_idle_sec;

    // Idle reaper.
    std::thread             m_reaper;
    std::condition_variable m_reaper_cv;
    std::mutex              m_reaper_mtx;
    bool                    m_stop = false;
};

/// Base64 (standard alphabet, padded) — PTY traffic is raw bytes that may
/// be invalid UTF-8, so it can't ride in a JSON string verbatim.
std::string base64_encode(const std::string& in);
std::string base64_decode(const std::string& in);

/// Install /api/v1/shell/* handlers. Every route is Admin-gated and behind
/// the http.shell.enabled master switch (read from ds per request). `mgr`,
/// `ds` and `auth` must outlive the router (owned by main).
void install_shell_handlers(Router& router,
                            data_store::Client* ds,
                            SessionStore* auth,
                            ShellManager* mgr);

} // namespace http_server

#endif
