/// L21/D4 — LwM2M DM Server unit tests (TDD).
/// Tests registration/lookup/routing via ?ep= query parameter.

#include <gtest/gtest.h>
#include "server/lwm2m/management.hpp"
#include "server/lwm2m/endpoint_registry.hpp"

using server::lwm2m::EndpointRegistry;
using server::lwm2m::ManagementRouter;

namespace {

TEST(ManagementRouterTest, ParseEpFromQuery) {
    EXPECT_EQ(ManagementRouter::parse_ep("ep=urn:dev:gateway-1"), "urn:dev:gateway-1");
    EXPECT_EQ(ManagementRouter::parse_ep("a=1&ep=dev-42&b=2"), "dev-42");
    EXPECT_EQ(ManagementRouter::parse_ep(""), "");
    EXPECT_EQ(ManagementRouter::parse_ep("no_ep_here"), "");
}

TEST(ManagementRouterTest, ResolveEndpoint) {
    EndpointRegistry reg;
    ManagementRouter router(reg);
    server::lwm2m::EndpointInfo info{"urn:dev:a", "10.9.0.10", 5001, true};
    reg.add(info);

    const auto* found = router.resolve("urn:dev:a");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->tun_ip, "10.9.0.10");
    EXPECT_TRUE(found->registered);
}

TEST(ManagementRouterTest, ResolveUnknownReturnsNull) {
    EndpointRegistry reg;
    ManagementRouter router(reg);
    EXPECT_EQ(router.resolve("urn:dev:unknown"), nullptr);
}

TEST(ManagementRouterTest, RegisterEndpoint) {
    EndpointRegistry reg;
    ManagementRouter router(reg);
    server::lwm2m::EndpointInfo info{"urn:dev:b", "10.9.0.11", 5002, false};
    reg.add(info);

    EXPECT_TRUE(router.set_registered("urn:dev:b", true));
    EXPECT_TRUE(reg.lookup_by_ep("urn:dev:b")->registered);
}

TEST(ManagementRouterTest, RouteRequestExtractsEp) {
    EndpointRegistry reg;
    ManagementRouter router(reg);
    server::lwm2m::EndpointInfo info{"urn:dev:c", "10.9.0.12", 5003, true};
    reg.add(info);

    auto result = router.route("GET /3/0/6?ep=urn:dev:c");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->target_ip, "10.9.0.12");
    EXPECT_EQ(result->path, "/3/0/6");
}
} // namespace
