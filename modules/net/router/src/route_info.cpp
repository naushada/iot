#include "route_info.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace net_router::route_info {

namespace {

using json = nlohmann::json;

std::string str_of(const json& e, const char* field) {
    if (!e.contains(field)) return {};
    const auto& v = e[field];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return {};
}

} // namespace

std::string parse_routes(const std::string& body) {
    json out = json::array();
    if (!body.empty()) {
        try {
            const auto j = json::parse(body);
            if (j.is_array()) {
                for (const auto& e : j) {
                    json r;
                    r["dst"]     = str_of(e, "dst");
                    r["gateway"] = str_of(e, "gateway");
                    r["dev"]     = str_of(e, "dev");
                    r["proto"]   = str_of(e, "protocol");
                    r["scope"]   = str_of(e, "scope");
                    r["prefsrc"] = str_of(e, "prefsrc");
                    r["metric"]  = str_of(e, "metric");
                    out.push_back(std::move(r));
                }
            }
        } catch (const std::exception&) {
            // Unparseable → empty table; the UI shows "no routes".
        }
    }
    return out.dump();
}

std::string parse_ifaces(const std::string& body) {
    json out = json::array();
    if (!body.empty()) {
        try {
            const auto j = json::parse(body);
            if (j.is_array()) {
                for (const auto& e : j) {
                    json r;
                    r["name"]  = str_of(e, "ifname");
                    r["state"] = str_of(e, "operstate");
                    r["mac"]   = str_of(e, "address");
                    std::string ip;
                    if (e.contains("addr_info") && e["addr_info"].is_array()) {
                        for (const auto& a : e["addr_info"]) {
                            if (!a.contains("family") || a["family"] != "inet") continue;
                            if (!a.contains("local") || !a["local"].is_string()) continue;
                            ip = a["local"].get<std::string>();
                            if (a.contains("prefixlen") && a["prefixlen"].is_number_integer())
                                ip += "/" + std::to_string(a["prefixlen"].get<int>());
                            break;
                        }
                    }
                    r["ip"] = ip;
                    out.push_back(std::move(r));
                }
            }
        } catch (const std::exception&) {
            // Unparseable → empty list.
        }
    }
    return out.dump();
}

std::string parse_resolv_conf(const std::string& text) {
    std::string out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string word, addr;
        ls >> word;
        if (word != "nameserver") continue;
        ls >> addr;
        if (addr.empty()) continue;
        if (!out.empty()) out += ",";
        out += addr;
    }
    return out;
}

std::string routes_json(shell::Runner runner) {
    if (!runner) return "[]";
    int rc = 0;
    auto body = runner({"ip", "-j", "route", "show"}, &rc);
    return rc == 0 ? parse_routes(body) : "[]";
}

std::string ifaces_json(shell::Runner runner) {
    if (!runner) return "[]";
    int rc = 0;
    auto body = runner({"ip", "-j", "addr", "show"}, &rc);
    return rc == 0 ? parse_ifaces(body) : "[]";
}

std::string dns_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_resolv_conf(ss.str());
}

} // namespace net_router::route_info
