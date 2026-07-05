#include "ddns/public_ip.hpp"

#include <cctype>

#include "ddns/http_client.hpp"

namespace ddns {

IpSource parse_ip_source(const std::string& s) {
    if (s == "dyndns2-auto") return IpSource::Dyndns2Auto;
    if (s == "cloud")        return IpSource::Cloud;
    return IpSource::Echo;   // default / "echo"
}

// Validate + normalize a dotted-quad IPv4. Trims surrounding whitespace (echo
// endpoints append a newline). Returns "" if not a well-formed IPv4.
std::string validate_ipv4(const std::string& raw) {
    // trim
    std::size_t b = 0, e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    const std::string s = raw.substr(b, e - b);
    if (s.empty() || s.size() > 15) return {};

    int octets = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        int val = 0, digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + (s[i] - '0');
            if (++digits > 3) return {};
            ++i;
        }
        if (digits == 0 || val > 255) return {};
        ++octets;
        if (i < s.size()) {
            if (s[i] != '.') return {};
            ++i;
            if (i == s.size()) return {};   // trailing dot
        }
    }
    return octets == 4 ? s : std::string{};
}

namespace {

class EchoDetector : public PublicIpDetector {
public:
    explicit EchoDetector(std::vector<std::string> endpoints)
        : m_endpoints(std::move(endpoints)) {
        if (m_endpoints.empty()) {
            m_endpoints = {
                "https://api.ipify.org",
                "https://checkip.amazonaws.com",
                "https://icanhazip.com",
            };
        }
    }

    std::optional<std::string> detect() override {
        for (const auto& url : m_endpoints) {
            HttpResponse resp;
            std::string err;
            if (!http_get(url, /*timeout*/ 15, resp, err)) continue;
            if (resp.status != 200) continue;
            std::string ip = validate_ipv4(resp.body);
            if (!ip.empty()) return ip;
        }
        return std::nullopt;
    }

private:
    std::vector<std::string> m_endpoints;
};

} // namespace

std::unique_ptr<PublicIpDetector>
make_echo_detector(std::vector<std::string> endpoints) {
    return std::make_unique<EchoDetector>(std::move(endpoints));
}

} // namespace ddns
