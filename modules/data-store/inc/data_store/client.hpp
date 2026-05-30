#ifndef __data_store_client_hpp__
#define __data_store_client_hpp__

/// Public client API for the data-store unix-socket service.
///
/// Applications link `libdatastore_client.a` and use this header to
/// talk to the running `ds-server` daemon. The header itself only
/// depends on the C++ standard library — ACE primitives live behind
/// a pimpl in `client.cpp`, so downstream apps don't need ACE on
/// their compile line (only on their link line, transitively from
/// the static lib).
///
/// Wire details live in proto.hpp; the design is in
/// modules/data-store/docs/design.md.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace data_store {

using KV = std::pair<std::string, std::string>;

/// Result of a connect() / recv_welcome() / send_request() call.
/// `ok` is true when the call completed successfully. On failure
/// `err` carries a single-line reason; `code` carries an errno when
/// the failure was a syscall error, or 0 for protocol/parse errors.
struct Status {
    bool        ok = true;
    int         code = 0;       // errno or 0
    std::string err;
};

/// Stream-socket client. Single-connection per instance; not
/// thread-safe. Internally uses ACE_LSOCK_Stream + ACE_Time_Value
/// for I/O so it shares the same timeout semantics as the rest of
/// the iot stack; the public ABI stays POSIX-clean via pimpl.
///
/// D1 exposes `connect()` + `recv_welcome()`; D2/D3/D5 add `set()`,
/// `get()`, `watch()`, `unwatch()`, and the async push receive loop.
class Client {
public:
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    /// Open the connection. `path` is the server's listening socket
    /// (defaults to data_store::proto::kDefaultSocketPath when empty).
    Status connect(std::string path = "");

    /// Read the single welcome line the server emits on accept.
    /// Returns the line (including trailing '\n') in `out`. Times
    /// out after `timeout_ms`.
    Status recv_welcome(std::string& out, std::int32_t timeout_ms = 1000);

    /// Read one '\n'-terminated line from the socket. Useful for
    /// consuming notify events (and for tests). Returns the line
    /// without the trailing newline in `out`.
    Status recv_line(std::string& out, std::int32_t timeout_ms = 1000);

    // --- ops --------------------------------------------------------

    /// `set` one or more key/value pairs atomically. Returns when
    /// the server's `{"ok":...}` ack is received; `id` correlates
    /// the response with the request.
    Status set(const std::vector<KV>& pairs, std::int32_t timeout_ms = 1000);
    Status set(const std::string& k, const std::string& v,
               std::int32_t timeout_ms = 1000) {
        return set(std::vector<KV>{{k, v}}, timeout_ms);
    }

    /// `get` one or more keys. `out` is filled in the same order as
    /// `keys`; missing keys come back with `has_value=false`.
    struct GetResult {
        std::string key;
        std::string value;
        bool        has_value = false;
    };
    Status get(const std::vector<std::string>& keys,
               std::vector<GetResult>&         out,
               std::int32_t                    timeout_ms = 1000);

    /// `register` for change notifications on one or more keys.
    /// Notifications arrive asynchronously and are read via
    /// `recv_line` / `recv_event` after the ack.
    Status watch(const std::vector<std::string>& keys,
                 std::int32_t                    timeout_ms = 1000);
    Status watch(const std::string& key, std::int32_t timeout_ms = 1000) {
        return watch(std::vector<std::string>{key}, timeout_ms);
    }

    /// `remove` one or more keys from the calling session's watch
    /// set (other sessions' watches on the same keys are unaffected).
    Status unwatch(const std::vector<std::string>& keys,
                   std::int32_t                    timeout_ms = 1000);
    Status unwatch(const std::string& key, std::int32_t timeout_ms = 1000) {
        return unwatch(std::vector<std::string>{key}, timeout_ms);
    }

    /// Parsed notification event. `prev_has_value` distinguishes a
    /// real previous value from a freshly-created key.
    struct Event {
        std::string key;
        std::string value;
        std::string prev;
        bool        prev_has_value = false;
    };
    /// Read the next notify line and parse it. Returns ok=false with
    /// err="not an event" if the line was a response, not a push.
    Status recv_event(Event& out, std::int32_t timeout_ms = 1000);

    /// Close the connection. Idempotent.
    void close();

    /// Underlying ACE_HANDLE / fd. `-1` when not connected. Tests
    /// use this; production code should not need it.
    int fd() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace data_store

#endif /* __data_store_client_hpp__ */
