#include "audit.hpp"

#include <nlohmann/json.hpp>

namespace http_server {

using nlohmann::json;

std::string audit_action_for_key(const std::string& key) {
    // The auditable operator/provisioning keys (db/set-driven). Anything else
    // (config tweaks, log levels, …) is intentionally not recorded — this log
    // is for tenant + device lifecycle, not every form save.
    if (key == "cloud.provision.request")          return "device.provision";
    if (key == "cloud.deprovision.request")        return "device.deprovision";
    if (key == "cloud.transfer.release.request")   return "device.transfer";
    if (key == "cloud.tenants")                    return "tenant.update";
    if (key == "cloud.bs.master.key")              return "bs.master.update";
    return {};
}

std::string append_audit(const std::string& array_json, const AuditEntry& e,
                         std::size_t cap) {
    json arr = json::parse(array_json, /*cb*/nullptr, /*allow_exceptions*/false);
    if (!arr.is_array()) arr = json::array();

    json rec = {
        {"ts",     e.ts},
        {"actor",  e.actor},
        {"tenant", e.tenant},
        {"action", e.action},
        {"target", e.target},
    };
    if (!e.detail.empty()) rec["detail"] = e.detail;

    // Newest-first: insert at the front.
    arr.insert(arr.begin(), std::move(rec));

    // Ring-buffer cap: keep the newest `cap` entries.
    if (cap > 0 && arr.size() > cap)
        arr.erase(arr.begin() + static_cast<long>(cap), arr.end());

    return arr.dump();
}

}  // namespace http_server
