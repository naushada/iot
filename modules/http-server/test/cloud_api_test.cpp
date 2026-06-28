/// L21/D7 — Cloud API handler unit tests (TDD).
///
/// Tests the /api/v1/cloud/* REST endpoints against the
/// endpoint registry + VPN registry + bootstrap provisioner
/// WITHOUT starting a real ds-server.

#include <gtest/gtest.h>

#include "handler.hpp"
#include "auth.hpp"
#include "parser.hpp"
#include "router.hpp"

#include "endpoint_registry.hpp"
#include "bootstrap.hpp"
#include "vpn_registry.hpp"

#include <nlohmann/json.hpp>

using http_server::HttpParser;
using http_server::HttpResponse;
using http_server::Router;
using json = nlohmann::json;

namespace {

// ── Fixture: sets up registries + cloud handlers ──────────────────

class CloudApiTest : public ::testing::Test {
protected:
    server::lwm2m::EndpointRegistry ep_reg;
    server::openvpn::VpnRegistry    vpn_reg{"10.9.0.0/24", 5001, 5100};
    server::lwm2m::BootstrapProvisioner provisioner{ep_reg, vpn_reg};
    Router router;

    void SetUp() override {
        http_server::install_cloud_handlers(router, &ep_reg, &provisioner);
    }

    HttpResponse get(const std::string& path) {
        HttpParser::Request req;
        req.method = "GET";
        req.path   = path;
        return router.route(req);
    }

    HttpResponse post(const std::string& path, const std::string& body) {
        HttpParser::Request req;
        req.method = "POST";
        req.path   = path;
        req.body   = body;
        return router.route(req);
    }
};

// ── 1. List endpoints (empty) ──────────────────────────────────────

TEST_F(CloudApiTest, ListEndpointsEmpty) {
    auto r = get("/api/v1/cloud/endpoints");
    EXPECT_EQ(r.status, 200);
    auto j = json::parse(r.body);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["endpoints"].size(), 0U);
}

// ── 2. Provision a device ──────────────────────────────────────────

TEST_F(CloudApiTest, ProvisionDevice) {
    auto r = post("/api/v1/cloud/endpoints",
                  R"({"endpoint":"urn:dev:gateway-1"})");
    EXPECT_EQ(r.status, 200);
    auto j = json::parse(r.body);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["endpoint"], "urn:dev:gateway-1");
    EXPECT_FALSE(j["tun_ip"].get<std::string>().empty());
    EXPECT_GT(j["proxy_port"].get<int>(), 0);
}

// ── 3. Provision duplicate fails ───────────────────────────────────

TEST_F(CloudApiTest, ProvisionDuplicateIsIdempotent) {
    // Re-provisioning an existing endpoint is idempotent (it refreshes the
    // credential) — it returns the SAME tunnel IP + proxy port, not a 400.
    auto r1 = post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:dup"})");
    auto r2 = post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:dup"})");
    EXPECT_EQ(r2.status, 200);
    auto j1 = json::parse(r1.body), j2 = json::parse(r2.body);
    EXPECT_TRUE(j2["ok"].get<bool>());
    EXPECT_EQ(j1["tun_ip"], j2["tun_ip"]);
    EXPECT_EQ(j1["proxy_port"], j2["proxy_port"]);
    EXPECT_EQ(ep_reg.count(), 1U);   // still one endpoint
}

// ── 4. Provision without endpoint returns 400 ──────────────────────

TEST_F(CloudApiTest, ProvisionMissingEndpointReturns400) {
    auto r = post("/api/v1/cloud/endpoints", R"({"foo":"bar"})");
    EXPECT_EQ(r.status, 400);
}

// ── 5. List endpoints after provision ──────────────────────────────

TEST_F(CloudApiTest, ListEndpointsAfterProvision) {
    post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:a"})");
    post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:b"})");

    auto r = get("/api/v1/cloud/endpoints");
    EXPECT_EQ(r.status, 200);
    auto j = json::parse(r.body);
    EXPECT_EQ(j["endpoints"].size(), 2U);
    EXPECT_EQ(j["count"], 2);
}

// ── 6. Get single endpoint detail ──────────────────────────────────

TEST_F(CloudApiTest, GetEndpointDetail) {
    post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:detail"})");
    // Use mock query via path?ep=... since Router does exact path matching
    HttpParser::Request req;
    req.method = "GET";
    req.path   = "/api/v1/cloud/endpoint";
    req.query["ep"] = "urn:dev:detail";
    auto r = router.route(req);
    EXPECT_EQ(r.status, 200);
    auto j = json::parse(r.body);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["endpoint"], "urn:dev:detail");
    EXPECT_FALSE(j["tun_ip"].get<std::string>().empty());
}

// ── 7. Get non-existent endpoint returns 404 ───────────────────────

TEST_F(CloudApiTest, GetNonExistentEndpointReturns404) {
    HttpParser::Request req;
    req.method = "GET";
    req.path   = "/api/v1/cloud/endpoint";
    req.query["ep"] = "urn:dev:nonexistent";
    auto r = router.route(req);
    EXPECT_EQ(r.status, 404);
}

// ── 8. De-provision an endpoint ────────────────────────────────────

TEST_F(CloudApiTest, DeprovisionEndpoint) {
    post("/api/v1/cloud/endpoints", R"({"endpoint":"urn:dev:to-remove"})");
    ASSERT_EQ(ep_reg.count(), 1U);
    HttpParser::Request req;
    req.method = "DELETE";
    req.path   = "/api/v1/cloud/endpoints";
    req.query["ep"] = "urn:dev:to-remove";
    auto r = router.route(req);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(json::parse(r.body)["ok"].get<bool>());
    EXPECT_EQ(ep_reg.count(), 0U);
}

// ── Multi-tenant read isolation (tdd-multi-tenant-cloud.md) ──────────

namespace {
server::lwm2m::EndpointInfo mk(const std::string& ep, std::uint16_t port,
                              const std::string& tenant) {
    server::lwm2m::EndpointInfo e;
    e.ep = ep; e.tun_ip = "10.9.0." + std::to_string(port - 5000);
    e.proxy_port = port; e.registered = true; e.tenant = tenant;
    return e;
}

// Build a router whose cloud handlers are tenant-scoped via `auth`, with three
// endpoints across acme / default / globex.
struct TenantFixture {
    server::lwm2m::EndpointRegistry     reg;
    server::openvpn::VpnRegistry        vpn{"10.9.0.0/24", 5001, 5100};
    server::lwm2m::BootstrapProvisioner prov{reg, vpn};
    http_server::SessionStore           auth;
    Router                              router;
    TenantFixture() {
        reg.add(mk("dev-acme",   5001, "acme"));
        reg.add(mk("dev-legacy", 5002, ""));        // untagged == default
        reg.add(mk("dev-globex", 5003, "globex"));
        http_server::install_cloud_handlers(router, &reg, &prov, &auth);
    }
    json list_as(const std::string& tenant) {
        std::string tok = auth.create_session("u", "user", "Admin", tenant);
        HttpParser::Request req;
        req.method = "GET";
        req.path   = "/api/v1/cloud/endpoints";
        req.headers["cookie"] = "iot-session=" + tok;
        return json::parse(router.route(req).body);
    }
};
} // namespace

TEST(CloudApiTenant, ListScopedToSessionTenant) {
    TenantFixture f;
    auto acme = f.list_as("acme");
    EXPECT_EQ(acme["count"], 1U);
    EXPECT_EQ(acme["endpoints"][0]["endpoint"], "dev-acme");

    auto def = f.list_as("default");
    EXPECT_EQ(def["count"], 1U);
    EXPECT_EQ(def["endpoints"][0]["endpoint"], "dev-legacy");   // untagged
}

TEST(CloudApiTenant, PlatformOperatorSeesAll) {
    TenantFixture f;
    auto all = f.list_as("*");
    EXPECT_EQ(all["count"], 3U);
}

TEST(CloudApiTenant, NoAuthIsLegacyUnfiltered) {
    // auth=nullptr → no filtering, exactly as before tenants existed.
    server::lwm2m::EndpointRegistry reg;
    server::openvpn::VpnRegistry    vpn{"10.9.0.0/24", 5001, 5100};
    server::lwm2m::BootstrapProvisioner prov{reg, vpn};
    Router router;
    reg.add(mk("dev-acme", 5001, "acme"));
    reg.add(mk("dev-legacy", 5002, ""));
    http_server::install_cloud_handlers(router, &reg, &prov, /*auth*/nullptr);
    HttpParser::Request req;
    req.method = "GET";
    req.path   = "/api/v1/cloud/endpoints";
    auto j = json::parse(router.route(req).body);
    EXPECT_EQ(j["count"], 2U);
}

TEST(CloudApiTenant, SingleEndpointHiddenAcrossTenant) {
    TenantFixture f;
    std::string tok = f.auth.create_session("u", "user", "Admin", "acme");
    HttpParser::Request req;
    req.method = "GET";
    req.path   = "/api/v1/cloud/endpoint";
    req.query["ep"] = "dev-globex";                 // belongs to globex
    req.headers["cookie"] = "iot-session=" + tok;
    auto r = f.router.route(req);
    EXPECT_EQ(r.status, 404);                        // not leaked to acme
}

} // namespace
