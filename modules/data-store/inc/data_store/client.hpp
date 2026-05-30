#ifndef __data_store_client_hpp__
#define __data_store_client_hpp__

/// Public client API for the data-store unix-socket service.
///
/// Applications link `libdatastore_client.a` and use this header to
/// talk to the running `ds-server` daemon. The library is intentionally
/// dependency-light: just POSIX sockets + std::string. No ACE, no
/// nlohmann::json in the public interface.
///
/// Wire details live in proto.hpp; the design is in
/// modules/data-store/docs/design.md.

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

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
/// thread-safe. Holds an AF_UNIX stream socket fd for the lifetime
/// of the object.
///
/// D1 only exposes `connect()` and `recv_welcome()` to prove the
/// round-trip; `set()`, `get()`, `watch()`, `unwatch()` land in
/// D3/D5.
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
    /// Returns a Status describing the outcome.
    Status connect(std::string path = "");

    /// Read the single welcome line the server emits on accept.
    /// Returns the line (including its trailing '\n') in `out` and
    /// `Status{ok=true}`; on parse / I/O error returns ok=false and
    /// leaves `out` untouched.
    Status recv_welcome(std::string& out, std::int32_t timeout_ms = 1000);

    /// Close the connection. Idempotent.
    void close();

    /// Underlying fd. Tests use this for poll(2); production code
    /// should not need it.
    int fd() const { return m_fd; }

private:
    int m_fd = -1;
};

} // namespace data_store

#endif /* __data_store_client_hpp__ */
