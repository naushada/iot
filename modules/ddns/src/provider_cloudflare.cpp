#include "ddns/provider.hpp"

#include <nlohmann/json.hpp>

#include "ddns/http_client.hpp"

/**
 * @file provider_cloudflare.cpp
 * @brief Cloudflare API backend (scoped Bearer token, Zone:DNS:Edit).
 *
 * 1. Resolve the A record id:
 *      GET /client/v4/zones/<zone>/dns_records?type=A&name=<host>
 * 2. Update it:
 *      PUT /client/v4/zones/<zone>/dns_records/<id>
 *      {"type":"A","name":<host>,"content":<ip>,"ttl":60,"proxied":false}
 * The record id is cached on the instance and re-resolved on a 404.
 *
 * Creds mapping: secret=api_token, target1=zone_id. Record name = host.
 */

namespace ddns {

namespace {

using json = nlohmann::json;

constexpr const char* kApi = "https://api.cloudflare.com/client/v4";

class CloudflareBackend : public ProviderBackend {
public:
    Result update(const std::string& host, const std::string& ip,
                  const Creds& c) override {
        const std::vector<std::string> hdrs = {
            "Authorization: Bearer " + c.secret,
            "Content-Type: application/json",
        };
        if (m_record_id.empty() || m_zone != c.target1 || m_name != host) {
            Result r = resolve_id(host, c, hdrs);
            if (!r.ok) return r;
        }

        json body = {
            {"type", "A"}, {"name", host}, {"content", ip},
            {"ttl", 60}, {"proxied", false},
        };
        const std::string url = std::string(kApi) + "/zones/" + c.target1 +
                                "/dns_records/" + m_record_id;
        HttpResponse resp;
        std::string err;
        if (!http_request("PUT", url, hdrs, "", body.dump(), 30, resp, err))
            return {false, 0, "transport: " + err};
        if (resp.status == 404) {                 // record deleted/rotated
            m_record_id.clear();
            Result r = resolve_id(host, c, hdrs);
            if (!r.ok) return r;
            if (!http_request("PUT", std::string(kApi) + "/zones/" + c.target1 +
                              "/dns_records/" + m_record_id, hdrs, "", body.dump(),
                              30, resp, err))
                return {false, 0, "transport: " + err};
        }
        if (cf_success(resp.body))
            return {true, static_cast<int>(resp.status), "ok"};
        return {false, static_cast<int>(resp.status), cf_error(resp)};
    }

    const char* name() const override { return "cloudflare"; }

private:
    // GET the record id by name; cache (zone,name,id).
    Result resolve_id(const std::string& host, const Creds& c,
                      const std::vector<std::string>& hdrs) {
        const std::string url = std::string(kApi) + "/zones/" + c.target1 +
                                "/dns_records?type=A&name=" + url_encode(host);
        HttpResponse resp;
        std::string err;
        if (!http_request("GET", url, hdrs, "", "", 30, resp, err))
            return {false, 0, "transport: " + err};
        try {
            json j = json::parse(resp.body);
            if (!j.value("success", false))
                return {false, static_cast<int>(resp.status), cf_error(resp)};
            auto& result = j.at("result");
            if (!result.is_array() || result.empty())
                return {false, static_cast<int>(resp.status),
                        "no A record named " + host + " in zone"};
            m_record_id = result[0].at("id").get<std::string>();
            m_zone = c.target1;
            m_name = host;
            return {true, 200, "resolved"};
        } catch (const std::exception& e) {
            return {false, static_cast<int>(resp.status),
                    std::string("json: ") + e.what()};
        }
    }

    static bool cf_success(const std::string& b) {
        try { return json::parse(b).value("success", false); }
        catch (...) { return false; }
    }
    static std::string cf_error(const HttpResponse& resp) {
        try {
            json j = json::parse(resp.body);
            if (j.contains("errors") && j["errors"].is_array() && !j["errors"].empty())
                return j["errors"][0].value("message", "cloudflare error");
        } catch (...) {}
        return "http " + std::to_string(resp.status);
    }

    std::string m_record_id, m_zone, m_name;
};

} // namespace

std::unique_ptr<ProviderBackend> make_cloudflare_backend() {
    return std::make_unique<CloudflareBackend>();
}

} // namespace ddns
