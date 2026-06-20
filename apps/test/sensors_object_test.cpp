#include <gtest/gtest.h>

#include <string>

#include "lwm2m_object_sensors.hpp"
#include "lwm2m_object_store.hpp"

using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::ResourceType;
using ::lwm2m::has_op;
namespace objects = ::lwm2m::objects;

TEST(Sensors, install_registers_all_ipso_objects) {
    ObjectStore store;
    objects::install_sensors(store);
    for (std::uint32_t oid : {3301u, 3303u, 3304u, 3315u, 3313u, 3334u}) {
        ASSERT_TRUE(store.find(oid)) << "object " << oid << " missing";
        EXPECT_TRUE(store.find(oid)->multipleInstance);
        EXPECT_TRUE(store.find(oid, 0, 5701)) << "units RID 5701 missing on " << oid;
    }
}

TEST(Sensors, scalar_objects_have_observable_value_resource) {
    ObjectStore store;
    objects::install_sensors(store);
    for (std::uint32_t oid : {3301u, 3303u, 3304u, 3315u}) {
        auto* r = store.find(oid, 0, 5700);
        ASSERT_TRUE(r) << "value RID 5700 missing on " << oid;
        EXPECT_EQ(r->type, ResourceType::Float);
        EXPECT_TRUE(has_op(r->ops, Operations::R));
        EXPECT_TRUE(r->observable);
    }
}

TEST(Sensors, triaxis_objects_have_three_axis_resources) {
    ObjectStore store;
    objects::install_sensors(store);
    for (std::uint32_t oid : {3313u, 3334u}) {
        for (std::uint32_t rid : {5702u, 5703u, 5704u}) {
            auto* r = store.find(oid, 0, rid);
            ASSERT_TRUE(r) << "axis RID " << rid << " missing on " << oid;
            EXPECT_TRUE(r->observable);
        }
    }
}

TEST(Sensors, unset_hook_reads_zero) {
    ObjectStore store;
    objects::install_sensors(store);   // no hooks
    EXPECT_EQ(store.find(3303, 0, 5700)->read(), "0");
    EXPECT_EQ(store.find(3313, 0, 5702)->read(), "0");
}

TEST(Sensors, hooks_drive_resource_reads) {
    objects::SensorHooks h;
    h.temperature = []() { return std::string("23.50"); };
    h.illuminance = []() { return std::string("65.44"); };
    h.accel_x     = []() { return std::string("-17"); };
    h.gyro_z      = []() { return std::string("42"); };

    ObjectStore store;
    objects::install_sensors(store, std::move(h));
    EXPECT_EQ(store.find(3303, 0, 5700)->read(), "23.50");   // Temperature
    EXPECT_EQ(store.find(3301, 0, 5700)->read(), "65.44");   // Illuminance
    EXPECT_EQ(store.find(3313, 0, 5702)->read(), "-17");     // Accel X
    EXPECT_EQ(store.find(3334, 0, 5704)->read(), "42");      // Gyro Z
    // an unset hook (humidity) still falls back to "0"
    EXPECT_EQ(store.find(3304, 0, 5700)->read(), "0");
}

TEST(Sensors, units_strings_are_correct) {
    ObjectStore store;
    objects::install_sensors(store);
    EXPECT_EQ(store.find(3303, 0, 5701)->read(), "Cel");
    EXPECT_EQ(store.find(3304, 0, 5701)->read(), "%RH");
    EXPECT_EQ(store.find(3315, 0, 5701)->read(), "Pa");
    EXPECT_EQ(store.find(3301, 0, 5701)->read(), "lx");
    EXPECT_EQ(store.find(3313, 0, 5701)->read(), "m/s2");
    EXPECT_EQ(store.find(3334, 0, 5701)->read(), "deg/s");
}
