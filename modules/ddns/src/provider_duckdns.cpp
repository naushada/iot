#include "ddns/provider.hpp"

#include "ddns/http_client.hpp"

/**
 * @file provider_duckdns.cpp
 * @brief DuckDNS backend — free, churn-tolerant, single GET.
 *
 * GET https://www.duckdns.org/update?domains=<sub>&token=<token>&ip=<ip>
 * (blank ip => server autodetect). Response body: "OK" / "KO".
 *
 * Creds mapping: secret=token, target1=domains (subdomain label(s)).
 */

namespace ddns {

namespace {

class DuckDnsBackend : public ProviderBackend {
public:
    Result update(const std::string& host, const std::string& ip,
                  const Creds& c) override {
        // Prefer the explicit domains label; fall back to the host's first label.
        std::string domains = c.target1;
        if (domains.empty()) {
            domains = host;
            std::size_t dot = domains.find('.');
            if (dot != std::string::npos) domains = domains.substr(0, dot);
        }
        const std::string url = "https://www.duckdns.org/update?domains=" +
                                url_encode(domains) + "&token=" + url_encode(c.secret) +
                                "&ip=" + url_encode(ip);
        HttpResponse resp;
        std::string err;
        if (!http_get(url, 30, resp, err)) {
            return {false, 0, "transport: " + err};
        }
        // Body is "OK" or "KO" (possibly with a trailing newline).
        std::string b = resp.body;
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r' || b.back() == ' '))
            b.pop_back();
        if (b == "OK")  return {true, static_cast<int>(resp.status), "OK"};
        return {false, static_cast<int>(resp.status), b.empty() ? "KO" : b};
    }

    const char* name() const override { return "duckdns"; }
};

} // namespace

std::unique_ptr<ProviderBackend> make_duckdns_backend() {
    return std::make_unique<DuckDnsBackend>();
}

} // namespace ddns
