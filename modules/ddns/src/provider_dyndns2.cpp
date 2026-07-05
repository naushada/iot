#include "ddns/provider.hpp"

#include "ddns/http_client.hpp"

/**
 * @file provider_dyndns2.cpp
 * @brief Generic dyndns2 backend (No-IP, Dynu, deSEC, IPv64, DynIP, routers…).
 *
 * GET https://<server>/nic/update?hostname=<host>&myip=<ip> with HTTP Basic
 * auth (user:token). Response body: "good <ip>" / "nochg <ip>" = success;
 * "nohost"/"badauth"/"notfqdn"/"abuse"/"!donator"/"911" = error.
 *
 * Creds mapping: user=username, secret=token, target1=server.
 */

namespace ddns {

namespace {

class Dyndns2Backend : public ProviderBackend {
public:
    Result update(const std::string& host, const std::string& ip,
                  const Creds& c) override {
        const std::string server = c.target1.empty() ? "members.dyndns.org" : c.target1;
        const std::string url = "https://" + server + "/nic/update?hostname=" +
                                url_encode(host) + "&myip=" + url_encode(ip);
        HttpResponse resp;
        std::string err;
        if (!http_request("GET", url, {}, c.user + ":" + c.secret, "",
                          30, resp, err)) {
            return {false, 0, "transport: " + err};
        }
        // dyndns2 signals success/failure in the BODY, not just the status.
        std::string b = resp.body;
        // take the first token (before whitespace)
        std::size_t sp = b.find_first_of(" \t\r\n");
        std::string code = (sp == std::string::npos) ? b : b.substr(0, sp);
        if (code == "good" || code == "nochg") {
            return {true, static_cast<int>(resp.status), code};
        }
        if (code.empty() && resp.status == 200) {
            return {true, 200, "ok"};   // some servers return bare 200
        }
        return {false, static_cast<int>(resp.status),
                code.empty() ? ("http " + std::to_string(resp.status)) : code};
    }

    const char* name() const override { return "dyndns2"; }
};

} // namespace

std::unique_ptr<ProviderBackend> make_dyndns2_backend() {
    return std::make_unique<Dyndns2Backend>();
}

} // namespace ddns
