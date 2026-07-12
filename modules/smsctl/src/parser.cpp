#include "smsctl/parser.hpp"

#include <algorithm>
#include <cctype>

namespace smsctl {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(
        std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

Command unknown(const char* why) {
    Command c;
    c.kind  = Kind::Unknown;
    c.error = why;
    return c;
}

} // namespace

const char* Command::keyword() const {
    switch (kind) {
        case Kind::Login:        return "LOGIN";
        case Kind::Logout:       return "LOGOUT";
        case Kind::Status:       return "STATUS";
        case Kind::Reboot:       return "REBOOT";
        case Kind::FactoryReset: return "FACTORY-RESET";
        case Kind::Apn:          return "APN";
        case Kind::RadioRestart: return "RADIO";
        case Kind::Wifi:         return "WIFI";
        case Kind::Unknown:      return "CMD";
        case Kind::NotACommand:  return "";
    }
    return "";
}

bool is_mutating(Kind k) {
    switch (k) {
        case Kind::Reboot:
        case Kind::FactoryReset:
        case Kind::Apn:
        case Kind::RadioRestart:
        case Kind::Wifi:
            return true;
        default:
            return false;
    }
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false, have_cur = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
            cur.push_back('"');          // \" → a literal quote
            have_cur = true;
            ++i;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            have_cur = true;             // "" is a real (empty) argument
            continue;
        }
        if (!in_quotes && is_space(c)) {
            if (have_cur) { out.push_back(cur); cur.clear(); have_cur = false; }
            continue;
        }
        cur.push_back(c);
        have_cur = true;
    }
    // An unterminated quote just runs to end-of-line — forgiving, because a
    // phone keyboard may have "smartened" the closing quote away.
    if (have_cur) out.push_back(cur);
    return out;
}

Command parse(const std::string& text) {
    const std::string body = trim(text);
    auto tok = tokenize(body);
    if (tok.empty()) return {};                       // NotACommand

    if (upper(tok[0]) != "IOT") return {};            // not for us — stay silent
    if (tok.size() < 2) return unknown("missing command");

    const std::string verb = upper(tok[1]);
    // Arguments after the keyword; NOT upper-cased (SSIDs, passwords and APNs
    // are case-sensitive).
    std::vector<std::string> args(tok.begin() + 2, tok.end());

    Command c;
    if (verb == "LOGIN") {
        if (args.size() != 2) return unknown("usage: IOT LOGIN <user> <password>");
        c.kind = Kind::Login;
        c.args = std::move(args);
        return c;
    }
    if (verb == "LOGOUT") {
        c.kind = Kind::Logout;
        return c;
    }
    if (verb == "STATUS") {
        c.kind = Kind::Status;
        return c;
    }
    if (verb == "REBOOT") {
        c.kind = Kind::Reboot;
        return c;
    }
    if (verb == "FACTORY-RESET" || verb == "FACTORYRESET") {
        if (args.size() > 1) return unknown("usage: IOT FACTORY-RESET [<nonce>]");
        c.kind = Kind::FactoryReset;
        c.args = std::move(args);        // empty = step 1 (mint a nonce)
        return c;
    }
    if (verb == "APN") {
        if (args.size() != 1) return unknown("usage: IOT APN <apn>");
        c.kind = Kind::Apn;
        c.args = std::move(args);
        return c;
    }
    if (verb == "RADIO") {
        if (args.size() != 1 || upper(args[0]) != "RESTART")
            return unknown("usage: IOT RADIO RESTART");
        c.kind = Kind::RadioRestart;
        return c;
    }
    if (verb == "WIFI") {
        if (args.empty() || args.size() > 2)
            return unknown("usage: IOT WIFI <ssid> [<psk>]");
        if (args[0].empty()) return unknown("WIFI: empty ssid");
        c.kind = Kind::Wifi;
        c.args = std::move(args);        // psk optional → open network
        return c;
    }
    // Deliberately does NOT echo the verb: an operator typo is harmless, but
    // echoing arbitrary sender-supplied text into a reply is not.
    return unknown("unknown command");
}

} // namespace smsctl
