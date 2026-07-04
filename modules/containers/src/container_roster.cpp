#include "container_roster.hpp"

#include <nlohmann/json.hpp>

namespace containers {

namespace {
/// Read a string field, tolerating null/absent (→ "").
std::string field(const nlohmann::json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    // Numeric (e.g. a legacy "pid") — not a roster config field, ignore.
    return {};
}
} // namespace

bool parse_roster(const std::string& json, std::vector<RosterEntry>& out) {
    out.clear();
    // An unset key reads back as "" (before the daemon has ever published) —
    // treat that as an empty roster, not a parse error.
    if (json.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_array()) return false;
    for (const auto& o : doc) {
        if (!o.is_object()) continue;
        RosterEntry e;
        e.name       = field(o, "name");
        e.image      = field(o, "image");
        e.image_id   = field(o, "imageId");
        e.size       = field(o, "size");
        e.net        = field(o, "net");
        e.subnet     = field(o, "subnet");
        e.mem        = field(o, "mem");
        e.cpus       = field(o, "cpus");
        e.entrypoint = field(o, "entrypoint");
        e.cmd        = field(o, "cmd");
        e.state      = field(o, "state");
        e.ip         = field(o, "ip");
        e.gateway    = field(o, "gateway");
        if (!e.name.empty()) out.push_back(std::move(e));
    }
    return true;
}

} // namespace containers
