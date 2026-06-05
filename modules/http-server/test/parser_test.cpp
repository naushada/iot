/// L18/D1 — HttpParser unit tests.
///
/// Pure logic: feed byte sequences, assert parse results. No ACE,
/// no sockets, no I/O. Each test constructs a parser, sets a
/// handler that captures the Request, feeds bytes, and asserts.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "parser.hpp"

using http_server::HttpParser;

namespace {

/// Feed a complete string and capture the Request.
struct Capture {
    HttpParser::Request req;
    std::string         response;
    bool                fired = false;

    void install(HttpParser& p) {
        p.set_handler([this](const HttpParser::Request& r) {
            req = r;
            fired = true;
            return "HTTP/1.1 200 OK\r\n\r\n";
        });
    }
};

} // namespace

// ─────────── D1-T1: simple GET, no body ────────────────────────

TEST(Parser, Parse_simple_get_no_body) {
    HttpParser p;
    Capture c;
    c.install(p);

    p.feed("GET / HTTP/1.1\r\n\r\n", 18);
    ASSERT_TRUE(p.done());
    ASSERT_TRUE(c.fired);
    EXPECT_EQ("GET", c.req.method);
    EXPECT_EQ("/",   c.req.path);
    EXPECT_EQ("/",   c.req.url);
    EXPECT_TRUE(c.req.body.empty());
    EXPECT_TRUE(c.req.query.empty());
    EXPECT_EQ("HTTP/1.1 200 OK\r\n\r\n", p.take_response());
}

// ─────────── D1-T2: POST with body ─────────────────────────────

TEST(Parser, Parse_post_with_body) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req =
        "POST /api/v1/db/set HTTP/1.1\r\n"
        "Content-Length: 15\r\n"
        "\r\n"
        "{\"key\":\"value\"}";          // 15 bytes
    p.feed(req.data(), req.size());

    ASSERT_TRUE(p.done());
    EXPECT_EQ("POST", c.req.method);
    EXPECT_EQ("/api/v1/db/set", c.req.path);
    EXPECT_EQ("{\"key\":\"value\"}", c.req.body);
    EXPECT_EQ("15", c.req.headers.at("content-length"));
}

// ─────────── D1-T3: query string parsing ────────────────────────

TEST(Parser, Parse_query_string) {
    HttpParser p;
    Capture c;
    c.install(p);

    p.feed("GET /api/v1/db/get?key=services.ds.state&timeout=30 HTTP/1.1\r\n\r\n", 64);

    ASSERT_TRUE(p.done());
    EXPECT_EQ("GET", c.req.method);
    EXPECT_EQ("/api/v1/db/get", c.req.path);
    EXPECT_EQ("/api/v1/db/get?key=services.ds.state&timeout=30", c.req.url);
    ASSERT_EQ(2u, c.req.query.size());
    EXPECT_EQ("services.ds.state", c.req.query.at("key"));
    EXPECT_EQ("30", c.req.query.at("timeout"));
}

// ─────────── D1-T4: Content-Length respected ───────────────────

TEST(Parser, Parse_content_length_header) {
    HttpParser p;
    Capture c;
    c.install(p);

    // Body is exactly 5 bytes: "hello"
    std::string req =
        "POST /x HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    p.feed(req.data(), req.size());

    ASSERT_TRUE(p.done());
    EXPECT_EQ("hello", c.req.body);

    // Extra bytes after body should not be consumed
    // (they stay in the buffer for the next request after reset).
}

// ─────────── D1-T5: incremental feed ────────────────────────────

TEST(Parser, Parse_incremental_feed) {
    HttpParser p;
    Capture c;
    c.install(p);

    // Feed 1 byte at a time.
    std::string req = "GET /x HTTP/1.1\r\n\r\n";
    for (std::size_t i = 0; i < req.size(); ++i) {
        p.feed(req.data() + i, 1);
    }

    ASSERT_TRUE(p.done());
    EXPECT_EQ("GET", c.req.method);
    EXPECT_EQ("/x",  c.req.path);
}

// ─────────── D1-T6: malformed method line → error ───────────────

TEST(Parser, Parse_malformed_method_line_returns_error) {
    HttpParser p;
    Capture c;
    c.install(p);

    p.feed("GARBAGE\r\n\r\n", 12);

    // The random bytes don't parse as a valid method line.
    EXPECT_TRUE(p.error());
    EXPECT_FALSE(p.done());
    EXPECT_FALSE(c.fired);
    EXPECT_FALSE(p.error_msg().empty());
}

// ─────────── D1-T7: Connection header captured ─────────────────

TEST(Parser, Parse_connection_keep_alive) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    p.feed(req.data(), req.size());

    ASSERT_TRUE(p.done());
    ASSERT_EQ(1u, c.req.headers.count("connection"));
    EXPECT_EQ("keep-alive", c.req.headers.at("connection"));
    ASSERT_EQ(1u, c.req.headers.count("host"));
    EXPECT_EQ("localhost", c.req.headers.at("host"));
}

// ─────────── D1-T8: reset for next request ─────────────────────

TEST(Parser, Parse_reset_for_next_request) {
    HttpParser p;
    Capture c;
    c.install(p);

    // First request.  "GET /first HTTP/1.1\r\n\r\n" = 23 bytes
    p.feed("GET /first HTTP/1.1\r\n\r\n",
           std::strlen("GET /first HTTP/1.1\r\n\r\n"));
    ASSERT_TRUE(p.done());
    EXPECT_EQ("/first", c.req.path);

    // Reset and parse a second request.
    p.reset();
    EXPECT_FALSE(p.done());

    std::string req2 =
        "POST /second HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "world";
    p.feed(req2.data(), req2.size());
    ASSERT_TRUE(p.done());
    EXPECT_EQ("POST", c.req.method);
    EXPECT_EQ("/second", c.req.path);
    EXPECT_EQ("world", c.req.body);
}

// ─────────── Transfer-Encoding: chunked ────────────────────────

TEST(Parser, Parse_chunked_body) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req =
        "POST /api/v1/db/set HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";
    p.feed(req.data(), req.size());
    ASSERT_TRUE(p.done());
    EXPECT_EQ("POST", c.req.method);
    EXPECT_EQ("Wikipedia", c.req.body);   // dechunked
}

TEST(Parser, Parse_chunked_incremental) {
    HttpParser p;
    Capture c;
    c.install(p);

    // Same body fed one byte at a time — exercises resumability across
    // every sub-state boundary (size line, data, CRLF, trailer).
    std::string req =
        "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n"
        "2\r\nde\r\n"
        "0\r\n\r\n";
    for (std::size_t i = 0; i < req.size() && !p.done(); ++i) {
        p.feed(req.data() + i, 1);
    }
    ASSERT_TRUE(p.done());
    EXPECT_EQ("abcde", c.req.body);
}

TEST(Parser, Parse_chunked_with_extension_and_trailer) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req =
        "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;name=value\r\nWiki\r\n"   // chunk extension is ignored
        "0\r\n"
        "X-Checksum: deadbeef\r\n"   // trailer header accepted + ignored
        "\r\n";
    p.feed(req.data(), req.size());
    ASSERT_TRUE(p.done());
    EXPECT_EQ("Wiki", c.req.body);
}

TEST(Parser, Parse_chunked_takes_precedence_over_content_length) {
    HttpParser p;
    Capture c;
    c.install(p);

    // RFC 7230 §3.3.3: when both are present, chunked wins, CL ignored.
    std::string req =
        "POST /x HTTP/1.1\r\n"
        "Content-Length: 999\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    p.feed(req.data(), req.size());
    ASSERT_TRUE(p.done());
    EXPECT_EQ("hello", c.req.body);
}

TEST(Parser, Parse_chunked_bad_size_errors) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req =
        "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "zz\r\n";   // not a hex chunk size
    p.feed(req.data(), req.size());
    EXPECT_TRUE(p.error());
    EXPECT_FALSE(p.done());
}

// ─────────── 411 Length Required (FUP-L18-3) ───────────────────

TEST(Parser, Post_without_length_is_411) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req = "POST /x HTTP/1.1\r\nHost: y\r\n\r\n";  // no CL, no TE
    p.feed(req.data(), req.size());
    EXPECT_TRUE(p.error());
    EXPECT_FALSE(p.done());
    EXPECT_EQ(p.error_status(), 411);
    EXPECT_FALSE(c.fired);
}

TEST(Parser, Put_without_length_is_411) {
    HttpParser p;
    Capture c;
    c.install(p);

    std::string req = "PUT /x HTTP/1.1\r\n\r\n";
    p.feed(req.data(), req.size());
    EXPECT_TRUE(p.error());
    EXPECT_EQ(p.error_status(), 411);
}

TEST(Parser, Post_with_zero_length_is_ok) {
    HttpParser p;
    Capture c;
    c.install(p);

    // Explicit Content-Length: 0 is a valid empty-body POST, not 411.
    std::string req = "POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    p.feed(req.data(), req.size());
    EXPECT_FALSE(p.error());
    EXPECT_TRUE(p.done());
    EXPECT_EQ(c.req.body, "");
}

TEST(Parser, Get_without_length_is_ok) {
    HttpParser p;
    Capture c;
    c.install(p);

    // GET has no body; absence of Content-Length is fine (not 411).
    std::string req = "GET /x HTTP/1.1\r\nHost: y\r\n\r\n";
    p.feed(req.data(), req.size());
    EXPECT_FALSE(p.error());
    EXPECT_TRUE(p.done());
}

TEST(Parser, Post_chunked_without_length_is_ok) {
    HttpParser p;
    Capture c;
    c.install(p);

    // chunked supplies the length, so no Content-Length is required.
    std::string req =
        "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\n\r\n";
    p.feed(req.data(), req.size());
    EXPECT_FALSE(p.error());
    EXPECT_TRUE(p.done());
    EXPECT_EQ(c.req.body, "abc");
}
