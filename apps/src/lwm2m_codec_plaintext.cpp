#include "lwm2m_codec_plaintext.hpp"

#include <cctype>

namespace lwm2m { namespace plaintext {

namespace {

bool is_decimal_integer(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        if (s.size() == 1) return false;
        ++i;
    }
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool is_decimal_float(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        if (s.size() == 1) return false;
        ++i;
    }
    bool sawDigit = false, sawDot = false, sawExp = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { sawDigit = true; continue; }
        if (c == '.' && !sawDot && !sawExp) { sawDot = true; continue; }
        if ((c == 'e' || c == 'E') && sawDigit && !sawExp) {
            sawExp = true;
            if (i + 1 < s.size() && (s[i+1] == '+' || s[i+1] == '-')) ++i;
            continue;
        }
        return false;
    }
    return sawDigit;
}

} // namespace

int encode(ResourceType type, const std::string& value, std::string& out) {
    switch (type) {
        case ResourceType::String:
        case ResourceType::Integer:
        case ResourceType::Float:
        case ResourceType::Time:
            out = value;
            return 0;
        case ResourceType::Boolean: {
            // Accept "0"/"1" as-is; accept "true"/"false" from the
            // application side and normalise to the spec form.
            if (value == "0" || value == "1") { out = value; return 0; }
            if (value == "true")  { out = "1"; return 0; }
            if (value == "false") { out = "0"; return 0; }
            return -1;
        }
        default:
            return -1;
    }
}

int decode(ResourceType type, const std::string& bytes, std::string& out) {
    switch (type) {
        case ResourceType::String:
            out = bytes;
            return 0;
        case ResourceType::Integer:
        case ResourceType::Time:
            if (!is_decimal_integer(bytes)) return -1;
            out = bytes;
            return 0;
        case ResourceType::Float:
            if (!is_decimal_float(bytes)) return -1;
            out = bytes;
            return 0;
        case ResourceType::Boolean:
            if (bytes != "0" && bytes != "1") return -1;
            out = bytes;
            return 0;
        default:
            return -1;
    }
}

}} // namespace lwm2m::plaintext
