#include "lwm2m_object_firmware.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include <ace/Log_Msg.h>

#include "lua_config.hpp"

namespace lwm2m { namespace objects {

namespace {

/// CoAP text/plain wire form of a lua_config value (RID 6/8/9 defaults).
std::string value_to_text(const iot::lua_config::ResourceValue& v) {
    if (auto* b = std::get_if<bool>(&v))        return *b ? "1" : "0";
    if (auto* n = std::get_if<long long>(&v))   return std::to_string(*n);
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    return {};
}

ResourceType value_to_type(const iot::lua_config::ResourceValue& v) {
    if (std::get_if<bool>(&v))      return ResourceType::Boolean;
    if (std::get_if<long long>(&v)) return ResourceType::Integer;
    if (std::get_if<std::string>(&v)) return ResourceType::String;
    return ResourceType::String;
}

Resource read_only(std::uint32_t rid, const std::string& name,
                   ResourceType type, std::string value) {
    Resource r;
    r.rid = rid; r.name = name; r.type = type; r.ops = Operations::R;
    r.read = [value]() { return value; };
    return r;
}

std::string join_path(const std::string& dir, const std::string& tail) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/') p.push_back('/');
    return p + tail;
}

} // namespace

int install_firmware_apply(ObjectStore& store,
                           const std::string& configDir,
                           FwHooks hooks) {
    auto hp     = std::make_shared<FwHooks>(std::move(hooks));
    auto staged = std::make_shared<std::string>();   // last Package URI

    ObjectInstance inst;
    inst.iid = 0;

    // RID 1 — Package URI (write): stage the .ipk URL (may carry ?sha256=).
    {
        Resource r;
        r.rid = 1; r.name = "Package URI";
        r.type = ResourceType::String; r.ops = Operations::W;
        r.write = [staged](const std::string& v) -> int {
            *staged = v;
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [fw-obj] %M %N:%l staged package URI (%d bytes)\n"),
                static_cast<int>(v.size())));
            return 0;
        };
        inst.resources[1] = std::move(r);
    }
    // RID 2 — Update (execute): launch the detached apply for the staged URI.
    {
        Resource r;
        r.rid = 2; r.name = "Update";
        r.type = ResourceType::None; r.ops = Operations::E;
        r.execute = [staged, hp](const std::string& /*args*/) -> int {
            if (staged->empty()) {
                ACE_ERROR_RETURN((LM_ERROR,
                    ACE_TEXT("%D [fw-obj] %M %N:%l update with no staged URI\n")), -1);
            }
            if (!hp->launch) {
                ACE_ERROR_RETURN((LM_ERROR,
                    ACE_TEXT("%D [fw-obj] %M %N:%l no launcher wired\n")), -1);
            }
            int rc = hp->launch(*staged);
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [fw-obj] %M %N:%l launched apply rc=%d\n"), rc));
            return rc;   // 0 → 2.04 Changed; updater advances State/Result async
        };
        inst.resources[2] = std::move(r);
    }
    // RID 3 — State (live from ds via hook).
    {
        Resource r;
        r.rid = 3; r.name = "State";
        r.type = ResourceType::Integer; r.ops = Operations::R;
        r.read = [hp]() {
            return std::to_string(hp->read_state ? hp->read_state() : 0);
        };
        inst.resources[3] = std::move(r);
    }
    // RID 5 — Update Result (live from ds via hook).
    {
        Resource r;
        r.rid = 5; r.name = "Update Result";
        r.type = ResourceType::Integer; r.ops = Operations::R;
        r.read = [hp]() {
            return std::to_string(hp->read_result ? hp->read_result() : 0);
        };
        inst.resources[5] = std::move(r);
    }
    // RID 7 — PkgVersion (live from ds via hook).
    {
        Resource r;
        r.rid = 7; r.name = "PkgVersion";
        r.type = ResourceType::String; r.ops = Operations::R;
        r.read = [hp]() {
            return hp->read_version ? hp->read_version() : std::string();
        };
        inst.resources[7] = std::move(r);
    }
    // RID 26 — Update Reason (vendor ext, live from ds via hook): the human
    // cause behind a terminal RID 5 result ("no .ipk in bundle", "sha256
    // mismatch"). Our lwm2m-dm polls it so the cloud can show WHY, not just
    // a bare result code; foreign servers never ask for it.
    {
        Resource r;
        r.rid = kFirmwareRidReason; r.name = "Update Reason";
        r.type = ResourceType::String; r.ops = Operations::R;
        r.read = [hp]() {
            return hp->read_reason ? hp->read_reason() : std::string();
        };
        inst.resources[kFirmwareRidReason] = std::move(r);
    }

    // RID 6/8/9 — PkgName / Protocol Support / Delivery Method, read-only
    // defaults from firmwareObject/0.lua (fall back if the file is absent).
    {
        auto m = iot::lua_config::load_object_resources(
            join_path(configDir, "firmwareObject/0.lua"));
        for (std::uint32_t rid : {6u, 8u, 9u}) {
            auto it = m.find(rid);
            if (it != m.end() && it->second.include) {
                inst.resources[rid] = read_only(rid, it->second.description,
                    value_to_type(it->second.value),
                    value_to_text(it->second.value));
            }
        }
        if (inst.resources.find(6) == inst.resources.end())
            inst.resources[6] = read_only(6, "PkgName", ResourceType::String, "iot");
        if (inst.resources.find(8) == inst.resources.end())
            inst.resources[8] = read_only(8, "Firmware Update Protocol Support", ResourceType::Integer, "1");
        if (inst.resources.find(9) == inst.resources.end())
            inst.resources[9] = read_only(9, "Firmware Update Delivery Method", ResourceType::Integer, "0");
    }

    ObjectDescriptor d;
    d.oid = kFirmwareObjectOid; d.name = "Firmware Update";
    d.urn = "urn:oma:lwm2m:oma:5:1.0";
    d.mandatory = false; d.multipleInstance = false;
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

}} // namespace lwm2m::objects
