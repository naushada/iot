#include "lwm2m_object_store.hpp"

namespace lwm2m {

void ObjectStore::add_object(ObjectDescriptor descriptor) {
    auto oid = descriptor.oid;
    m_objects[oid] = std::move(descriptor);
}

bool ObjectStore::has(std::uint32_t oid) const {
    return m_objects.find(oid) != m_objects.end();
}

bool ObjectStore::has(std::uint32_t oid, std::uint32_t iid) const {
    auto it = m_objects.find(oid);
    return it != m_objects.end() && it->second.instances.find(iid) != it->second.instances.end();
}

bool ObjectStore::has(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid) const {
    auto* inst = find(oid, iid);
    return inst && inst->resources.find(rid) != inst->resources.end();
}

ObjectDescriptor* ObjectStore::find(std::uint32_t oid) {
    auto it = m_objects.find(oid);
    return it == m_objects.end() ? nullptr : &it->second;
}

const ObjectDescriptor* ObjectStore::find(std::uint32_t oid) const {
    auto it = m_objects.find(oid);
    return it == m_objects.end() ? nullptr : &it->second;
}

ObjectInstance* ObjectStore::find(std::uint32_t oid, std::uint32_t iid) {
    auto* desc = find(oid);
    if (!desc) return nullptr;
    auto it = desc->instances.find(iid);
    return it == desc->instances.end() ? nullptr : &it->second;
}

const ObjectInstance* ObjectStore::find(std::uint32_t oid, std::uint32_t iid) const {
    auto* desc = find(oid);
    if (!desc) return nullptr;
    auto it = desc->instances.find(iid);
    return it == desc->instances.end() ? nullptr : &it->second;
}

Resource* ObjectStore::find(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid) {
    auto* inst = find(oid, iid);
    if (!inst) return nullptr;
    auto it = inst->resources.find(rid);
    return it == inst->resources.end() ? nullptr : &it->second;
}

const Resource* ObjectStore::find(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid) const {
    auto* inst = find(oid, iid);
    if (!inst) return nullptr;
    auto it = inst->resources.find(rid);
    return it == inst->resources.end() ? nullptr : &it->second;
}

} // namespace lwm2m
