#include "mgmt_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace openvpn_client::mgmt {

namespace {

/// Split `s` on `,` into tokens. No empty-token suppression — OpenVPN's
/// STATE messages legitimately have trailing empty fields
/// (`...,1194,,`), so the caller can index them positionally.
std::vector<std::string> comma_split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

/// True iff `s` starts with `prefix`. C++17-friendly (string_view's
/// starts_with arrived in C++20).
bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

/// Strip trailing CR (some openvpn builds CRLF the mgmt socket on
/// Windows; harmless to handle defensively everywhere).
void rstrip_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

} // namespace

void Parser::feed(std::string_view bytes) {
    m_buf.append(bytes.data(), bytes.size());
    // Scan for newlines; emit one Event per complete line.
    std::size_t start = 0;
    for (std::size_t i = 0; i < m_buf.size(); ++i) {
        if (m_buf[i] != '\n') continue;
        std::string line = m_buf.substr(start, i - start);
        rstrip_cr(line);
        emit(std::move(line));
        start = i + 1;
    }
    if (start > 0) {
        m_buf.erase(0, start);
    }
}

std::optional<Event> Parser::next() {
    if (m_events.empty()) return std::nullopt;
    Event e = std::move(m_events.front());
    m_events.pop_front();
    return e;
}

void Parser::emit(std::string line) {
    Event ev;

    // Async events have a `>EVENT:body` shape.
    if (!line.empty() && line.front() == '>') {
        // Find the colon that separates `>EVENT` from `body`. If
        // there's no colon at all (`>SOMETHING-WITHOUT-COLON`), treat
        // the whole thing past `>` as a tag with empty body.
        const auto colon = line.find(':');
        std::string tag = (colon == std::string::npos)
                          ? line.substr(1)
                          : line.substr(1, colon - 1);
        std::string body = (colon == std::string::npos)
                           ? std::string{}
                           : line.substr(colon + 1);

        if      (tag == "INFO")       ev.kind = Event::Kind::Banner;
        else if (tag == "STATE")      ev.kind = Event::Kind::State;
        else if (tag == "PUSH_REPLY") ev.kind = Event::Kind::PushReply;
        else if (tag == "BYTECOUNT")  ev.kind = Event::Kind::ByteCount;
        else if (tag == "LOG")        ev.kind = Event::Kind::Log;
        else if (tag == "HOLD")       ev.kind = Event::Kind::Hold;
        else if (tag == "FATAL")      ev.kind = Event::Kind::Fatal;
        else if (tag == "ECHO")       ev.kind = Event::Kind::Echo;
        else if (tag == "PASSWORD")   ev.kind = Event::Kind::Password;
        else if (tag == "NEED-OK")    ev.kind = Event::Kind::NeedOk;
        else if (tag == "CLIENT")     ev.kind = Event::Kind::Client;
        else                          ev.kind = Event::Kind::Unknown;

        ev.raw = std::move(body);

        // Fields population: STATE / PUSH_REPLY / BYTECOUNT all
        // comma-separate their payload. Other event kinds put their
        // raw body in `raw` only.
        if (ev.kind == Event::Kind::State     ||
            ev.kind == Event::Kind::PushReply ||
            ev.kind == Event::Kind::ByteCount) {
            ev.fields = comma_split(ev.raw);
        }
    }
    else if (starts_with(line, "SUCCESS:")) {
        ev.kind = Event::Kind::SuccessReply;
        // body starts after "SUCCESS:" — and openvpn typically adds
        // a leading space ("SUCCESS: pid=1234"), which we keep
        // verbatim so consumers can parse the form they expect.
        ev.raw = line.substr(8);
    }
    else if (starts_with(line, "ERROR:")) {
        ev.kind = Event::Kind::ErrorReply;
        ev.raw = line.substr(6);
    }
    else if (line == "END") {
        ev.kind = Event::Kind::EndMarker;
        // raw stays empty
    }
    else if (!line.empty()) {
        ev.kind = Event::Kind::DataLine;
        ev.raw = std::move(line);
    }
    else {
        // Empty line — drop, don't enqueue.
        return;
    }

    m_events.push_back(std::move(ev));
}

std::pair<std::string, std::string>
split_push_option(std::string_view option) {
    // Trim leading whitespace (PUSH_REPLY can be `dhcp-option DNS …`
    // or rarely `,dhcp-option DNS …` after a trailing comma; the
    // caller's comma_split handles the latter but a stray leading
    // space is still possible).
    std::size_t start = 0;
    while (start < option.size() &&
           (option[start] == ' ' || option[start] == '\t')) {
        ++start;
    }
    option.remove_prefix(start);

    auto space = option.find(' ');
    if (space == std::string_view::npos) {
        // Bare option like "redirect-gateway" — name only, no value.
        return { std::string(option), std::string{} };
    }
    return { std::string(option.substr(0, space)),
             std::string(option.substr(space + 1)) };
}

} // namespace openvpn_client::mgmt
