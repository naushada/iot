#ifndef __http_server_parser_hpp__
#define __http_server_parser_hpp__

/// In-house HTTP/1.1 request parser (L18/D1).
///
/// Push-based, callback-driven. No external dependencies — pure
/// C++17 with std::string. Following RFC 7230 minimal subset:
/// method-line, headers, and either a Content-Length-delimited or
/// Transfer-Encoding: chunked body.
///
/// Usage:
///   HttpParser p;
///   p.set_handler([](const HttpParser::Request& r) {
///       return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
///   });
///   p.feed(data, len);
///   if (p.done()) { send(p.take_response()); p.reset(); }
///
/// Threading: NOT thread-safe. One parser per connection (reactor
/// thread owns it). Caller serialises feed()/reset()/done().

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace http_server {

class HttpParser {
public:
    struct Request {
        std::string                          method;
        std::string                          url;       // full URL with query
        std::string                          path;      // path component only
        std::map<std::string, std::string>   query;     // parsed query params
        std::map<std::string, std::string>   headers;   // all headers, lowercased keys
        std::string                          body;      // raw body bytes
    };

    /// Callback type: receives the parsed Request, returns the HTTP
    /// response string (status-line + headers + body).
    using Handler = std::function<std::string(const Request&)>;

    HttpParser() = default;

    /// Set the completion handler. Called when a full request is
    /// parsed. The return value is captured and available via
    /// take_response().
    void set_handler(Handler h) { m_handler = std::move(h); }

    /// Feed bytes into the parser. Returns the number of bytes
    /// consumed. Fewer bytes than `len` may be consumed if the
    /// parser reaches Done state (the remainder belongs to the
    /// next request on a keep-alive connection).
    std::size_t feed(const char* data, std::size_t len);

    /// True when a complete request has been parsed and the handler
    /// has fired.
    bool done() const { return m_state == State::Done; }

    /// True when the parser encountered an unrecoverable error.
    /// The caller should close the connection.
    bool error() const { return m_error; }

    /// The error message from the last failed parse.
    const std::string& error_msg() const { return m_error_msg; }

    /// HTTP status the caller should return for the current error: 400 for
    /// a malformed request, 411 when a body-bearing method lacks both
    /// Content-Length and Transfer-Encoding. Valid only when error().
    int error_status() const { return m_error_status; }

    /// Take the handler's response string. Valid only when done().
    std::string take_response() { return std::move(m_response); }

    /// Reset for the next request on the same connection. Preserves
    /// the handler registration.
    void reset();

    /// Peek at the parsed request. Valid only when done().
    const Request& request() const { return m_req; }

private:
    enum class State { MethodLine, Headers, Body, Done };

    // Sub-state for Transfer-Encoding: chunked bodies (RFC 7230 §4.1).
    enum class Chunk { Size, Data, DataCrlf, Trailer };

    State       m_state = State::MethodLine;
    bool        m_error = false;
    int         m_error_status = 400;             // 400 default, 411 on length-required
    std::string m_error_msg;
    Request     m_req;
    Handler     m_handler;
    std::string m_response;      // captured from handler
    std::string m_buf;           // rolling buffer
    bool        m_has_content_length = false;      // Content-Length header seen
    std::size_t m_content_length = 0;
    std::size_t m_body_read      = 0;

    bool        m_chunked        = false;          // Transfer-Encoding: chunked
    Chunk       m_chunk_state    = Chunk::Size;
    std::size_t m_chunk_remaining = 0;             // bytes left in current chunk

    // Parse helpers — each consumes from m_buf, returns true when
    // that stage is complete.
    bool parse_method_line();
    bool parse_headers();
    bool parse_body();
    bool parse_chunked();        // dechunk into m_req.body, resumable

    /// Fire the handler (if set) and capture the response.
    void fire_handler();

    /// Parse "key=value&..." into m_req.query.
    static std::map<std::string, std::string> parse_query(
        std::string_view qs);

    /// URL-decode a percent-encoded string.
    static std::string url_decode(std::string_view s);
};

} // namespace http_server

#endif /* __http_server_parser_hpp__ */
