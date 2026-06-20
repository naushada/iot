#include <gtest/gtest.h>

#include <string>

#include "lwm2m_object_store.hpp"
#include "lwm2m_object_stubs.hpp"

using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::ResourceType;
using ::lwm2m::has_op;
namespace objects = ::lwm2m::objects;

TEST(Location, InstallsObject6Resources) {
    ObjectStore store;
    objects::install_location(store);
    ASSERT_TRUE(store.find(6));
    for (std::uint32_t rid : {0u, 1u, 2u, 5u, 6u}) {
        auto* r = store.find(6, 0, rid);
        ASSERT_TRUE(r) << "RID " << rid << " missing";
        EXPECT_TRUE(has_op(r->ops, Operations::R));
        EXPECT_TRUE(r->observable);
    }
    EXPECT_EQ(store.find(6, 0, 0)->type, ResourceType::Float);   // Latitude
    EXPECT_EQ(store.find(6, 0, 5)->type, ResourceType::Time);    // Timestamp
}

TEST(Location, UnsetHooksReadZero) {
    ObjectStore store;
    objects::install_location(store);
    EXPECT_EQ(store.find(6, 0, 0)->read(), "0.0");   // Latitude default
    EXPECT_EQ(store.find(6, 0, 6)->read(), "0.0");   // Speed default
    EXPECT_EQ(store.find(6, 0, 5)->read(), "0");     // Timestamp default
}

TEST(Location, HooksDriveReads) {
    objects::LocationHooks h;
    h.latitude  = []() { return std::string("48.117300"); };
    h.longitude = []() { return std::string("11.516667"); };
    h.altitude  = []() { return std::string("545.4"); };
    h.speed     = []() { return std::string("41.5"); };

    ObjectStore store;
    objects::install_location(store, std::move(h));
    EXPECT_EQ(store.find(6, 0, 0)->read(), "48.117300");
    EXPECT_EQ(store.find(6, 0, 1)->read(), "11.516667");
    EXPECT_EQ(store.find(6, 0, 2)->read(), "545.4");
    EXPECT_EQ(store.find(6, 0, 6)->read(), "41.5");
    // unset timestamp hook still falls back to "0"
    EXPECT_EQ(store.find(6, 0, 5)->read(), "0");
}

TEST(Location, CanonicalObjectsThreadsLocationHooks) {
    objects::LocationHooks h;
    h.latitude = []() { return std::string("12.34"); };
    ObjectStore store;
    objects::install_canonical_objects(store, "/no-such-dir", {}, {}, {}, std::move(h));
    ASSERT_TRUE(store.find(6, 0, 0));
    EXPECT_EQ(store.find(6, 0, 0)->read(), "12.34");
}
