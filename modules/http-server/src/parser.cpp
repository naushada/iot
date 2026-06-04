#include "http_server/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace http_server {

namespace {

/// Consume up to `max` bytes from `src` into `dst`, stopping at
/// `delim`. Returns number of bytes consumed (including delim).
/// If delim not found within max, returns 0.
std::size_t consume_until(std::string_view src, std::string& dst,
                           char delim, std::size_t max = 8192) {
    for (std::size_t i = 0; i < max && i < src.size(); ++i) {
        if (src[i] == delim) {
            dst.append(src.data(), i);
            return i + 1;
        }
    }
    return 0;
}

/// Consume exactly `n` bytes if available.
bool consume_exact(std::string_view src, std::string& dst,
                   std::size_t n) {
    if (src.size() < n) return false;
    dst.append(src.data(), n);
    return true;
}

/// Trim leading/trailing whitespace in-place.
void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

/// Lowercase in-place.
void lower(std::string& s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

/// Return true if s starts with prefix (case-insensitive).
bool istarts_with(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

} // namespace

std::size_t HttpParser::feed(const char* data, std::size_t len) {
    if (m_error) return 0;
    m_buf.append(data, len);
    std::size_t consumed = 0;

    while (!m_error && m_state != State::Done && !m_buf.empty()) {
        bool advanced = false;
        switch (m_state) {
            case State::MethodLine:
                advanced = parse_method_line();
                break;
            case State::Headers:
                advanced = parse_headers();
                break;
            case State::Body:
                advanced = parse_body();
                break;
            case State::Done:
                break;
        }
        if (!advanced) break;   // need more data
    }

    // If we reached Done, the response is captured. Calculate
    // consumed = original_len - remaining in buffer.
    // The remainder stays in m_buf for the next request (keep-alive).
    if (m_state == State::Done) {
        consumed = len;  // approximate — caller uses done() to gate
        // Trim consumed prefix from m_buf so leftover is for next req.
        // We track by remembering how much we've consumed from m_buf.
    }
    return consumed;
}

bool HttpParser::parse_method_line() {
    // Find \r\n
    auto crlf = m_buf.find("\r\n");
    if (crlf == std::string::npos) {
        // Allow just \n for tolerance
        crlf = m_buf.find('\n');
        if (crlf == std::string::npos) return false;
    }
    std::string line = m_buf.substr(0, crlf);
    std::size_t skip = crlf;
    if (m_buf[skip] == '\r') ++skip;
    ++skip;  // past \n
    m_buf.erase(0, skip);

    trim(line);
    if (line.empty()) return false;  // empty line, wait for more

    // Parse: METHOD SP URL SP HTTP/1.x
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) {
        m_error = true;
        m_error_msg = "bad method line: no space after method";
        return false;
    }
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        m_error = true;
        m_error_msg = "bad method line: no HTTP version";
        return false;
    }

    m_req.method = line.substr(0, sp1);
    m_req.url    = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Parse path + query from URL
    auto qmark = m_req.url.find('?');
    if (qmark != std::string::npos) {
        m_req.path = m_req.url.substr(0, qmark);
        m_req.query = parse_query(
            std::string_view(m_req.url).substr(qmark + 1));
    } else {
        m_req.path = m_req.url;
    }

    // Validate method — only GET and POST in v1
    if (m_req.method != "GET" && m_req.method != "POST") {
        // Still parse it — let the router return 405
    }

    m_state = State::Headers;
    return true;
}

bool HttpParser::parse_headers() {
    while (true) {
        auto crlf = m_buf.find("\r\n");
        if (crlf == std::string::npos) {
            crlf = m_buf.find('\n');
            if (crlf == std::string::npos) return false;
        }

        std::string line = m_buf.substr(0, crlf);
        std::size_t skip = crlf;
        if (m_buf[skip] == '\r') ++skip;
        ++skip;
        m_buf.erase(0, skip);

        trim(line);
        if (line.empty()) {
            // Empty line = end of headers. Transfer-Encoding: chunked takes
            // precedence over Content-Length (RFC 7230 §3.3.3).
            auto te = m_req.headers.find("transfer-encoding");
            if (te != m_req.headers.end()) {
                std::string v = te->second;
                lower(v);
                if (v.find("chunked") != std::string::npos) {
                    m_chunked = true;
                    m_chunk_state = Chunk::Size;
                    m_state = State::Body;
                    return true;
                }
            }
            if (m_content_length > 0) {
                m_state = State::Body;
            } else {
                m_state = State::Done;
                fire_handler();
            }
            return true;
        }

        // Parse "Name: Value"
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;  // skip malformed

        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        trim(name); trim(value);
        lower(name);

        m_req.headers[name] = value;

        if (name == "content-length") {
            m_content_length = static_cast<std::size_t>(
                std::strtoul(value.c_str(), nullptr, 10));
        }
    }
}

// Cap the decoded body so a hostile/endless chunk stream can't exhaust
// memory. Applies to both framings.
static constexpr std::size_t kMaxBody = 8u * 1024 * 1024;  // 8 MiB

bool HttpParser::parse_body() {
    if (m_chunked) return parse_chunked();

    std::size_t remaining = m_content_length - m_body_read;
    std::size_t take = std::min(remaining, m_buf.size());
    if (m_req.body.size() + take > kMaxBody) {
        m_error = true;
        m_error_msg = "body exceeds limit";
        return false;
    }
    m_req.body.append(m_buf.data(), take);
    m_body_read += take;
    m_buf.erase(0, take);

    if (m_body_read >= m_content_length) {
        m_state = State::Done;
        fire_handler();
        return true;
    }
    return false;  // need more data
}

// Dechunk into m_req.body. Resumable across feed() calls: m_chunk_state +
// m_chunk_remaining persist between partial reads. Returns true if it made
// progress (consumed bytes or reached Done); false when it needs more data.
bool HttpParser::parse_chunked() {
    bool advanced = false;
    for (;;) {
        switch (m_chunk_state) {
        case Chunk::Size: {
            auto crlf = m_buf.find("\r\n");
            if (crlf == std::string::npos) {
                crlf = m_buf.find('\n');
                if (crlf == std::string::npos) return advanced;  // need more
            }
            std::string line = m_buf.substr(0, crlf);
            std::size_t skip = crlf + (m_buf[crlf] == '\r' ? 2 : 1);
            m_buf.erase(0, skip);
            advanced = true;

            // chunk-size [ ";" chunk-ext ] — keep only the hex size.
            auto semi = line.find(';');
            std::string hex = (semi == std::string::npos) ? line
                                                          : line.substr(0, semi);
            trim(hex);
            char* end = nullptr;
            unsigned long sz = std::strtoul(hex.c_str(), &end, 16);
            if (hex.empty() || end == hex.c_str()) {
                m_error = true;
                m_error_msg = "chunked: bad chunk size";
                return advanced;
            }
            if (sz == 0) {
                m_chunk_state = Chunk::Trailer;       // last-chunk
            } else {
                if (m_req.body.size() + sz > kMaxBody) {
                    m_error = true;
                    m_error_msg = "chunked: body exceeds limit";
                    return advanced;
                }
                m_chunk_remaining = sz;
                m_chunk_state = Chunk::Data;
            }
            break;
        }
        case Chunk::Data: {
            std::size_t take = std::min(m_chunk_remaining, m_buf.size());
            m_req.body.append(m_buf.data(), take);
            m_buf.erase(0, take);
            m_chunk_remaining -= take;
            if (take) advanced = true;
            if (m_chunk_remaining > 0) return advanced;  // need more
            m_chunk_state = Chunk::DataCrlf;
            break;
        }
        case Chunk::DataCrlf: {
            // The CRLF that terminates a chunk's data.
            if (m_buf.empty()) return advanced;
            if (m_buf[0] == '\r') {
                if (m_buf.size() < 2) return advanced;   // wait for the \n
                m_buf.erase(0, 2);
            } else if (m_buf[0] == '\n') {
                m_buf.erase(0, 1);
            } else {
                m_error = true;
                m_error_msg = "chunked: missing CRLF after chunk data";
                return advanced;
            }
            advanced = true;
            m_chunk_state = Chunk::Size;
            break;
        }
        case Chunk::Trailer: {
            // Optional trailer headers, then a blank line ends the message.
            auto crlf = m_buf.find("\r\n");
            if (crlf == std::string::npos) {
                crlf = m_buf.find('\n');
                if (crlf == std::string::npos) return advanced;
            }
            std::string line = m_buf.substr(0, crlf);
            std::size_t skip = crlf + (m_buf[crlf] == '\r' ? 2 : 1);
            m_buf.erase(0, skip);
            advanced = true;
            trim(line);
            if (line.empty()) {
                m_state = State::Done;
                fire_handler();
                return advanced;
            }
            // Trailer header lines are accepted but ignored in v1.
            break;
        }
        }
    }
}

void HttpParser::fire_handler() {
    if (m_handler) {
        m_response = m_handler(m_req);
    }
}

void HttpParser::reset() {
    m_state = State::MethodLine;
    m_error = false;
    m_error_msg.clear();
    m_req = Request{};
    m_response.clear();
    // Keep any leftover bytes in m_buf for the next request.
    m_content_length = 0;
    m_body_read = 0;
    m_chunked = false;
    m_chunk_state = Chunk::Size;
    m_chunk_remaining = 0;
}

std::map<std::string, std::string> HttpParser::parse_query(
        std::string_view qs) {
    std::map<std::string, std::string> out;
    while (!qs.empty()) {
        auto amp = qs.find('&');
        std::string_view pair = qs.substr(0, amp);

        auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            std::string key(pair.substr(0, eq));
            std::string val(pair.substr(eq + 1));
            out[url_decode(key)] = url_decode(val);
        } else {
            out[url_decode(pair)] = "";
        }

        if (amp == std::string_view::npos) break;
        qs = qs.substr(amp + 1);
    }
    return out;
}

std::string HttpParser::url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i+1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i+2]))) {
            char hex[3] = {s[i+1], s[i+2], '\0'};
            out += static_cast<char>(std::strtoul(hex, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace http_server
