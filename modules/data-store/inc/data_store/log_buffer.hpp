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
///   data_store::LogBuffer g_log("cloudd", "log.cloudd.text");
///   …
///   int main() {
///       g_log.start();  // register ACE callback after ACE is ready
///       …
///       g_log.flush(ds);
///   }

#include <memory>
#include <string>

namespace data_store {

class Client;  // forward — defined in data_store/client.hpp

class LogBuffer {
public:
    /// Start capturing ACE log output.
    /// @param daemon  Short name inserted into each log line
    /// @param ds_key  Data-store key written by flush() (may be changed
    ///                later with set_key())
    LogBuffer(const std::string& daemon, const std::string& ds_key);

    /// Register the ACE log callback.  Must be called from main()
    /// after ACE is initialised — NOT during static initialisation.
    void start();

    /// Unregister the callback. Flush one last time before destroying.
    ~LogBuffer();

    LogBuffer(const LogBuffer&)            = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;
    LogBuffer(LogBuffer&&)                 = delete;
    LogBuffer& operator=(LogBuffer&&)      = delete;

    /// Push accumulated ring-buffer lines into the data store.
    /// Best-effort — errors are silently dropped.
    void flush(Client& ds);

    /// Change the data-store key for subsequent flushes (useful when
    /// the same binary runs in different roles, e.g. lwm2m-bs vs dm).
    void set_key(const std::string& key);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace data_store

#endif  // __data_store_log_buffer_hpp__