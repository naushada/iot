#include <gtest/gtest.h>

#include "lwm2m_object_store.hpp"

using ::lwm2m::ObjectDescriptor;
using ::lwm2m::ObjectInstance;
using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::Resource;
using ::lwm2m::ResourceType;
using ::lwm2m::has_op;

TEST(ObjectStore, REQ_OBJ_002_descriptor_metadata) {
    ObjectDescriptor d;
    d.oid              = 3;
    d.name             = "Device";
    d.urn              = "urn:oma:lwm2m:oma:3:1.1";
    d.mandatory        = true;
    d.multipleInstance = false;

    Resource manufacturer;
    manufacturer.rid        = 0;
    manufacturer.name       = "Manufacturer";
    manufacturer.type       = ResourceType::String;
    manufacturer.ops        = Operations::R;
    manufacturer.mandatory  = false;
    manufacturer.observable = false;
    d.resourceTemplates[0]  = manufacturer;

    Resource reboot;
    reboot.rid    = 4;
    reboot.name   = "Reboot";
    reboot.type   = ResourceType::None;
    reboot.ops    = Operations::E;
    d.resourceTemplates[4] = reboot;

    ObjectStore store;
    store.add_object(d);

    ASSERT_TRUE(store.has(3));
    const auto* found = store.find(3);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ("Device", found->name);
    EXPECT_TRUE(found->mandatory);
    EXPECT_FALSE(found->multipleInstance);
    EXPECT_TRUE(has_op(found->resourceTemplates.at(0).ops, Operations::R));
    EXPECT_TRUE(has_op(found->resourceTemplates.at(4).ops, Operations::E));
}

TEST(ObjectStore, REQ_OBJ_002_instance_lookup) {
    ObjectStore store;
    ObjectDescriptor d;
    d.oid = 1;
    d.name = "Server";
    ObjectInstance inst;
    inst.iid = 0;
    Resource ssid;
    ssid.rid  = 0;
    ssid.type = ResourceType::Integer;
    ssid.ops  = Operations::R;
    inst.resources[0] = ssid;
    d.instances[0] = inst;
    store.add_object(d);

    EXPECT_TRUE(store.has(1));
    EXPECT_TRUE(store.has(1, 0));
    EXPECT_TRUE(store.has(1, 0, 0));
    EXPECT_FALSE(store.has(1, 0, 99));
    EXPECT_FALSE(store.has(1, 99));

    auto* r = store.find(1, 0, 0);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(ResourceType::Integer, r->type);
}

TEST(ObjectStore, D2_attributes_keyed_by_short_server_id) {
    // Decision D2: attribute records are keyed by Short Server ID so a
    // future multi-server build does not need to widen the record layout.
    Resource r;
    r.rid  = 13;
    r.type = ResourceType::Time;
    r.ops  = Operations::R;

    ::lwm2m::NotificationAttributes a;
    a.shortServerId = 1;
    a.pmin = 5;
    a.pmax = 30;
    r.attrs.push_back(a);

    ::lwm2m::NotificationAttributes b;
    b.shortServerId = 7;
    b.pmin = 10;
    b.pmax = 60;
    r.attrs.push_back(b);

    // v1 only uses one entry, but the data model supports many.
    ASSERT_EQ(2u, r.attrs.size());
    EXPECT_EQ(1u, r.attrs[0].shortServerId);
    EXPECT_EQ(7u, r.attrs[1].shortServerId);
}
