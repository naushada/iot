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

namespace data_store {

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
