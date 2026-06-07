#ifndef __data_store_log_buffer_hpp__
#define __data_store_log_buffer_hpp__

/// Reusable ACE log ring-buffer for daemon log lines.
///
/// Every daemon that wants its ACE_DEBUG / ACE_ERROR output to appear
/// in the cloud UI log viewer creates one LogBuffer instance, passing
/// a short daemon name ("cloudd", "httpd", "lwm2m") and the data-store
/// key to flush into ("log.cloudd.text", …).
///
/// The constructor registers an ACE_Log_Msg_Callback that captures
/// every ACE log line into an internal ring buffer (last 200 lines).
/// Call `flush(ds)` periodically to push accumulated lines into the
/// data store so the cloud UI long-polls them.
///
/// Usage (one instance per daemon, before any ACE_DEBUG calls):
///
///   #include "data_store/log_buffer.hpp"
///   data_store::LogBuffer g_log("cloudd", "log.cloudd.text",
///                               "log.level.cloudd");
///   …
///   int main() {
///       g_log.start();  // register ACE callback after ACE is ready
///       g_log.apply_level(ds);  // read log.level.cloudd → log.level
///       …
///       g_log.flush(ds);
///   }
///   // In event loop, on log.level / log.level.cloudd watch:
///   g_log.apply_level(ds);  // hot-reload

#include <memory>
#include <string>

namespace data_store {

class Client;  // forward — defined in data_store/client.hpp

class LogBuffer {
public:
    /// Start capturing ACE log output.
    /// @param daemon     Short name inserted into each log line
    /// @param log_key    Data-store key written by flush() (may be changed
    ///                   later with set_key())
    /// @param level_key  Per-daemon log-level key ("log.level.cloudd").
    ///                   apply_level() reads this key first, then falls
    ///                   back to the global "log.level".
    LogBuffer(const std::string& daemon, const std::string& log_key,
              const std::string& level_key);

    /// Convenience — level_key defaults to "log.level" (no per-daemon key).
    LogBuffer(const std::string& daemon, const std::string& log_key);

    /// Register the ACE log callback.  Must be called from main()
    /// after ACE is initialised — NOT during static initialisation.
    void start();

    /// Attach the (already-started) log callback to the CALLING thread.
    /// ACE_Log_Msg is thread-specific: a thread that runs its own reactor
    /// (e.g. the LwM2M UDP/CoAP svc() thread) does NOT inherit the callback
    /// registered by start() on the main thread, so its ACE_DEBUG/ACE_ERROR
    /// output bypasses the ring buffer and never reaches log.*.text. Such a
    /// thread must call this once at entry. No-op if start() hasn't run.
    /// Process-wide: targets the most recently start()ed LogBuffer.
    static void attach_current_thread();

    /// Unregister the callback. Flush one last time before destroying.
    ~LogBuffer();

    LogBuffer(const LogBuffer&)            = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;
    LogBuffer(LogBuffer&&)                 = delete;
    LogBuffer& operator=(LogBuffer&&)      = delete;

    /// Push accumulated ring-buffer lines into the data store.
    /// Only writes if there is new data since the last flush AND
    /// at least `min_bytes` of new text has accumulated (default 0
    /// = flush any new data). Best-effort — errors are silently dropped.
    /// Safe to call as often as you like — cheap no-op most of the time.
    void flush(Client& ds, std::size_t min_bytes = 0);

    /// Drive flush() automatically on a dedicated ACE reactor timer — same
    /// pattern as StatsPublisher (a private ACE_Reactor run on its own
    /// ACE_Task thread). Call once, after start(), instead of flushing from
    /// the daemon's own loop / a std::thread. `ds` must outlive close().
    /// @param interval_sec  flush cadence (default 10s).
    /// @param min_bytes     only write when this many new bytes accrued.
    int  open(Client& ds, int interval_sec = 10, std::size_t min_bytes = 0);

    /// Cancel the timer, do a final flush, end the loop and join the thread.
    /// Idempotent; also called by the destructor (but call it explicitly
    /// while `ds` is still alive — e.g. before main() returns).
    void close();

    /// Change the data-store log-text key for subsequent flushes
    /// (useful when the same binary runs in different roles).
    void set_log_key(const std::string& key);

    /// Change the log-level key for subsequent apply_level() calls.
    void set_level_key(const std::string& key);

    /// Read the per-daemon log-level key (given at construction) and
    /// fall back to "log.level".  Updates ACE_Log_Msg::priority_mask.
    /// Call at startup and on watch events for hot-reload.
    void apply_level(Client& ds);

    /// Append a line directly to the ring buffer (bypasses ACE).
    /// Use for critical messages when the ACE callback isn't reliable.
    void append(const std::string& line);

    /// Number of lines currently buffered (for tests).
    std::size_t line_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace data_store

#endif  // __data_store_log_buffer_hpp__