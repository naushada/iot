#include "ddns/provider.hpp"

#include "ddns/http_client.hpp"
#include "ddns/sigv4.hpp"

/**
 * @file provider_route53.cpp
 * @brief AWS Route53 backend (SigV4-signed ChangeResourceRecordSets UPSERT).
 *
 * POST https://route53.amazonaws.com/2013-04-01/hostedzone/<zone>/rrset
 * with a ChangeBatch UPSERT (A / TTL 60), signed with SigV4 (service route53,
 * region us-east-1 — Route53 is a global endpoint).
 *
 * Creds mapping: user=access_key_id, secret=secret_access_key,
 * target1=hosted_zone_id. Record name = host.
 */

namespace ddns {

namespace {

constexpr const char* kHost    = "route53.amazonaws.com";
constexpr const char* kRegion  = "us-east-1";
constexpr const char* kService = "route53";

std::string strip_zone_prefix(std::string z) {
    const std::string pfx = "/hostedzone/";
    if (z.rfind(pfx, 0) == 0) z = z.substr(pfx.size());
    if (!z.empty() && z.front() == '/') z.erase(0, 1);
    return z;
}

std::string xml_body(const std::string& host, const std::string& ip) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<ChangeResourceRecordSetsRequest xmlns=\"https://route53.amazonaws.com/doc/2013-04-01/\">"
        "<ChangeBatch><Changes><Change>"
        "<Action>UPSERT</Action>"
        "<ResourceRecordSet>"
        "<Name>" + host + "</Name>"
        "<Type>A</Type>"
        "<TTL>60</TTL>"
        "<ResourceRecords><ResourceRecord><Value>" + ip + "</Value></ResourceRecord></ResourceRecords>"
        "</ResourceRecordSet>"
        "</Change></Changes></ChangeBatch>"
        "</ChangeResourceRecordSetsRequest>";
}

// Pull <Message>…</Message> out of a Route53 ErrorResponse (best effort).
std::string xml_message(const std::string& body) {
    auto b = body.find("<Message>");
    if (b == std::string::npos) return {};
    b += 9;
    auto e = body.find("</Message>", b);
    if (e == std::string::npos) return {};
    return body.substr(b, e - b);
}

class Route53Backend : public ProviderBackend {
public:
    Result update(const std::string& host, const std::string& ip,
                  const Creds& c) override {
        const std::string zone = strip_zone_prefix(c.target1);
        const std::string path = "/2013-04-01/hostedzone/" + zone + "/rrset";
        const std::string url  = std::string("https://") + kHost + path;
        const std::string body = xml_body(host, ip);

        std::vector<std::string> hdrs = sigv4_headers(
            "POST", kHost, path, /*query*/ "", body,
            kRegion, kService, c.user, c.secret, sigv4_now_amz_date());
        hdrs.push_back("Content-Type: application/xml");

        HttpResponse resp;
        std::string err;
        if (!http_request("POST", url, hdrs, "", body, 30, resp, err))
            return {false, 0, "transport: " + err};

        if (resp.status == 200 || resp.status == 201)
            return {true, static_cast<int>(resp.status), "ok"};
        std::string msg = xml_message(resp.body);
        return {false, static_cast<int>(resp.status),
                msg.empty() ? ("http " + std::to_string(resp.status)) : msg};
    }

    const char* name() const override { return "route53"; }
};

} // namespace

std::unique_ptr<ProviderBackend> make_route53_backend() {
    return std::make_unique<Route53Backend>();
}

} // namespace ddns
