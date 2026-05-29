#ifndef __lwm2m_object_store_hpp__
#define __lwm2m_object_store_hpp__

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file lwm2m_object_store.hpp
 * @brief Runtime object/resource model for the LwM2M stack.
 *
 * L1 deliverable per apps/docs/lwm2m-design.md §3.1 and apps/docs/lwm2m-rdd.md
 * REQ-OBJ-002. Types only — no behavior — so callers and tests can compile
 * against the final shape while later phases (L3 / L5 / L7) fill in lookup,
 * Read/Write/Execute dispatch, and observer bookkeeping.
 *
 * Wire-format types (LwM2MObject / LwM2MObjectData) intentionally remain in
 * lwm2m_codec_tlv.hpp — those describe TLV bytes; the types here describe
 * the live device-side model.
 */

namespace lwm2m {

/// Spec data types defined in OMA-TS-LightweightM2M_Core-V1_1_1 §C.
enum class ResourceType : std::uint8_t {
    None,
    String,
    Integer,
    Float,
    Boolean,
    Opaque,
    Time,
    ObjLink,
};

/// Resource operation modes from the OMA registry.
enum class Operations : std::uint8_t {
    None    = 0,
    R       = 1 << 0,
    W       = 1 << 1,
    E       = 1 << 2,
    RW      = R | W,
    RWE     = R | W | E,
};

constexpr Operations operator|(Operations a, Operations b) {
    return static_cast<Operations>(static_cast<std::uint8_t>(a) |
                                   static_cast<std::uint8_t>(b));
}
constexpr bool has_op(Operations set, Operations op) {
    return (static_cast<std::uint8_t>(set) &
            static_cast<std::uint8_t>(op)) != 0;
}

/**
 * @brief Per-resource notification attributes carried by Write-Attributes
 * and consumed by the Observe engine.
 *
 * Per the D2 decision the attribute set is held per Short Server ID so the
 * v1 single-server build and the future multi-server build share one
 * datatype.
 */
struct NotificationAttributes {
    std::uint16_t shortServerId{0};       ///< D2: key, not "first server seen"
    std::uint32_t pmin{0};                ///< seconds
    std::uint32_t pmax{0};                ///< seconds; 0 == unset
    double        gt{0.0};
    double        lt{0.0};
    double        st{0.0};
    bool          hasGt{false};
    bool          hasLt{false};
    bool          hasSt{false};
};

/**
 * @brief A single LwM2M Resource as held in the runtime store.
 *
 * Read/Write/Execute are std::function so concrete bindings (object 3 device
 * info, custom application resources) can be injected without touching the
 * codec layer. Empty function == operation unsupported; the dispatcher will
 * reply 4.05 Method Not Allowed.
 */
struct Resource {
    std::uint32_t                                 rid{0};
    std::string                                   name;
    ResourceType                                  type{ResourceType::None};
    Operations                                    ops{Operations::None};
    bool                                          multiple{false};   ///< Resource Instance variant
    bool                                          mandatory{false};
    bool                                          observable{false};

    /// Returns the resource value as a TLV-ready payload (for non-multi resources).
    std::function<std::string()>                  read;
    /// Accepts a TLV-decoded value; returns 0 on success, CoAP error code on failure.
    std::function<int(const std::string&)>        write;
    /// Triggered by POST /{oid}/{iid}/{rid}; returns CoAP error code or 0.
    std::function<int(const std::string&)>        execute;

    /// L7 (Observe) bookkeeping. Filled in by the observer engine, keyed by
    /// shortServerId (D2). Not used in L1.
    std::vector<NotificationAttributes>           attrs;
};

/**
 * @brief One Object Instance: a numbered slot under an Object Descriptor.
 */
struct ObjectInstance {
    std::uint32_t                                  iid{0};
    std::unordered_map<std::uint32_t, Resource>    resources;     ///< keyed by rid
};

/**
 * @brief Object-level metadata + the live set of instances.
 *
 * `multipleInstance` mirrors the OMA registry "MultipleInstances" column;
 * `mandatory` mirrors the "Mandatory" column. The codec layer uses neither
 * — they exist for the FSM (Create / Delete admission) in L5.
 */
struct ObjectDescriptor {
    std::uint32_t                                  oid{0};
    std::string                                    name;
    std::string                                    urn;            ///< e.g. "urn:oma:lwm2m:oma:3:1.1"
    bool                                           multipleInstance{false};
    bool                                           mandatory{false};
    /// Default operations / type / multiplicity for resources of this object;
    /// individual Resource entries override per RID.
    std::unordered_map<std::uint32_t, Resource>    resourceTemplates;
    /// Live instances. `std::map` so iteration yields ascending iid order
    /// (Discover / link-format printers rely on that).
    std::map<std::uint32_t, ObjectInstance>        instances;
};

/**
 * @brief The runtime object/resource database for one LwM2M endpoint.
 *
 * One ObjectStore per Client (the device's own state) and one per server-side
 * registered client (the server's view of the device). The two never share
 * an instance.
 */
class ObjectStore {
public:
    ObjectStore() = default;
    ~ObjectStore() = default;

    /// Register an object descriptor (Bootstrap-Write commit point and
    /// startup-time registration of mandatory objects).
    void   add_object(ObjectDescriptor descriptor);

    /// Test whether oid/iid/rid is present. Filled in during L5.
    bool   has(std::uint32_t oid) const;
    bool   has(std::uint32_t oid, std::uint32_t iid) const;
    bool   has(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid) const;

    /// Mutating lookups. nullptr if absent. Filled in during L5.
    ObjectDescriptor* find(std::uint32_t oid);
    ObjectInstance*   find(std::uint32_t oid, std::uint32_t iid);
    Resource*         find(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid);

    /// Const variants for read-only paths (Read / Discover).
    const ObjectDescriptor* find(std::uint32_t oid) const;
    const ObjectInstance*   find(std::uint32_t oid, std::uint32_t iid) const;
    const Resource*         find(std::uint32_t oid, std::uint32_t iid, std::uint32_t rid) const;

    /// All registered objects, ascending by oid.
    const std::map<std::uint32_t, ObjectDescriptor>& objects() const { return m_objects; }

private:
    std::map<std::uint32_t, ObjectDescriptor> m_objects;
};

} // namespace lwm2m

#endif /*__lwm2m_object_store_hpp__*/
