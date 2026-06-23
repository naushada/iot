#include "registry.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace containers {

namespace {

using json = nlohmann::json;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Extract `key=value` from a challenge fragment. Value may be double-quoted
// ("realm=\"https://...\"") or a bare token up to the next comma/space.
std::string extract_param(const std::string& s, const std::string& key) {
    const std::string needle = key + "=";
    auto pos = s.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos < s.size() && s[pos] == '"') {
        ++pos;
        auto end = s.find('"', pos);
        if (end == std::string::npos) return {};
        return s.substr(pos, end - pos);
    }
    auto end = s.find_first_of(", \t", pos);
    return s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// Conservative URL-encoding: percent-encode anything outside a safe set. The
// safe set keeps the characters Docker scope/service strings rely on
// (':' '/' etc.) intact while escaping spaces and other unsafe bytes.
std::string url_encode(const std::string& s) {
    static const std::string safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "-._~:/?&=,+@";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (safe.find(static_cast<char>(c)) != std::string::npos) {
            out.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

Descriptor parse_descriptor(const json& d) {
    Descriptor out;
    if (d.contains("mediaType") && d["mediaType"].is_string())
        out.media_type = d["mediaType"].get<std::string>();
    if (d.contains("digest") && d["digest"].is_string())
        out.digest = d["digest"].get<std::string>();
    if (d.contains("size") && d["size"].is_number_integer())
        out.size = d["size"].get<long long>();
    return out;
}

} // namespace

BearerChallenge parse_bearer_challenge(const std::string& www_authenticate) {
    BearerChallenge c;
    const std::string lower = to_lower(www_authenticate);
    auto bpos = lower.find("bearer");
    if (bpos == std::string::npos) return c;       // no Bearer scheme
    // Params are case-insensitive keys with original-case values; extract from
    // the original string starting at the Bearer position.
    const std::string frag = www_authenticate.substr(bpos);
    c.realm   = extract_param(frag, "realm");
    c.service = extract_param(frag, "service");
    c.scope   = extract_param(frag, "scope");
    c.ok = !c.realm.empty();
    return c;
}

std::string build_token_url(const BearerChallenge& c) {
    if (c.realm.empty()) return {};
    std::string url = c.realm;
    char sep = (url.find('?') == std::string::npos) ? '?' : '&';
    if (!c.service.empty()) {
        url += sep;
        url += "service=" + url_encode(c.service);
        sep = '&';
    }
    if (!c.scope.empty()) {
        url += sep;
        url += "scope=" + url_encode(c.scope);
    }
    return url;
}

std::string parse_token_response(const std::string& body) {
    try {
        auto j = json::parse(body);
        if (j.contains("token") && j["token"].is_string())
            return j["token"].get<std::string>();
        if (j.contains("access_token") && j["access_token"].is_string())
            return j["access_token"].get<std::string>();
    } catch (...) {
    }
    return {};
}

bool manifest_is_index(const std::string& body) {
    try {
        auto j = json::parse(body);
        return j.contains("manifests") && j["manifests"].is_array();
    } catch (...) {
        return false;
    }
}

std::vector<PlatformDescriptor> parse_manifest_index(const std::string& body) {
    std::vector<PlatformDescriptor> out;
    try {
        auto j = json::parse(body);
        if (!j.contains("manifests") || !j["manifests"].is_array()) return out;
        for (const auto& m : j["manifests"]) {
            PlatformDescriptor p;
            static_cast<Descriptor&>(p) = parse_descriptor(m);
            if (m.contains("platform") && m["platform"].is_object()) {
                const auto& pl = m["platform"];
                if (pl.contains("os") && pl["os"].is_string())
                    p.os = pl["os"].get<std::string>();
                if (pl.contains("architecture") && pl["architecture"].is_string())
                    p.arch = pl["architecture"].get<std::string>();
                if (pl.contains("variant") && pl["variant"].is_string())
                    p.variant = pl["variant"].get<std::string>();
            }
            // Skip attestation / "unknown" placeholder manifests.
            if (p.os == "unknown" || p.arch == "unknown") continue;
            out.push_back(std::move(p));
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

int select_platform(const std::vector<PlatformDescriptor>& entries,
                    const std::string& os, const std::string& arch,
                    const std::string& variant) {
    int fallback = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.os != os || e.arch != arch) continue;
        if (e.variant == variant) return static_cast<int>(i);  // exact
        // os+arch match with a differing variant — remember the first as a
        // fallback (covers want-variant="" vs an entry tagged e.g. "v8").
        if (fallback < 0) fallback = static_cast<int>(i);
    }
    return fallback;
}

ImageManifest parse_image_manifest(const std::string& body) {
    ImageManifest out;
    try {
        auto j = json::parse(body);
        if (!j.contains("config") || !j.contains("layers")) return out;
        out.config = parse_descriptor(j["config"]);
        for (const auto& l : j["layers"])
            out.layers.push_back(parse_descriptor(l));
        out.ok = !out.config.digest.empty();
    } catch (...) {
        out = ImageManifest{};
    }
    return out;
}

bool is_valid_digest(const std::string& digest) {
    static const std::string prefix = "sha256:";
    if (digest.size() != prefix.size() + 64) return false;
    if (digest.compare(0, prefix.size(), prefix) != 0) return false;
    for (std::size_t i = prefix.size(); i < digest.size(); ++i) {
        char c = digest[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

std::string blob_path(const std::string& root, const std::string& digest) {
    if (!is_valid_digest(digest)) return {};
    return root + "/blobs/sha256/" + digest.substr(7);
}

} // namespace containers
