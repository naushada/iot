#include "image_ref.hpp"

namespace containers {

namespace {

constexpr const char* kDefaultTag = "latest";

// Docker rule: the first path component is a registry host (rather than a
// Docker Hub user/org) only if it contains a '.' (domain), a ':' (port), or is
// exactly "localhost".
bool looks_like_registry(const std::string& s) {
    return s == "localhost" ||
           s.find('.') != std::string::npos ||
           s.find(':') != std::string::npos;
}

} // namespace

bool parse_image_ref(const std::string& ref, ImageRef& out) {
    out = ImageRef{};

    // 0. Forgive a pasted command. An image ref never contains whitespace, so
    //    if the input has any — e.g. "docker pull nginx", "podman pull nginx" —
    //    take the LAST whitespace-separated token as the ref (and trim). A bare
    //    "nginx" is unchanged.
    std::string s = ref;
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;       // all-whitespace / empty
    s = s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    auto ws = s.find_last_of(" \t");
    if (ws != std::string::npos) s = s.substr(ws + 1);
    if (s.empty()) return false;

    // 1. Peel an optional registry off the front (component before the first
    //    '/', if it looks like a host). This must run before the tag split so a
    //    registry port (host:5000/...) is not mistaken for a tag.
    std::string registry;
    std::string name = s;               // repository[:tag][@digest]
    auto slash = name.find('/');
    if (slash != std::string::npos && looks_like_registry(name.substr(0, slash))) {
        registry = name.substr(0, slash);
        name = name.substr(slash + 1);
    }

    // 2. Peel an optional @digest.
    auto at = name.find('@');
    if (at != std::string::npos) {
        out.digest = name.substr(at + 1);
        name = name.substr(0, at);
        if (out.digest.empty()) return false;
    }

    // 3. Peel an optional :tag — only a ':' that falls after the last '/'
    //    (any registry port was already removed in step 1).
    auto last_slash = name.rfind('/');
    auto colon = name.rfind(':');
    if (colon != std::string::npos &&
        (last_slash == std::string::npos || colon > last_slash)) {
        out.tag = name.substr(colon + 1);
        name = name.substr(0, colon);
        if (out.tag.empty()) return false;
    }

    if (name.empty()) return false;
    out.repository = name;

    // 4. Docker Hub normalization: rewrite the host and add the implicit
    //    library/ namespace for single-segment names.
    if (registry.empty() || registry == "docker.io" || registry == "index.docker.io") {
        out.registry = kDockerRegistryHost;
        if (out.repository.find('/') == std::string::npos)
            out.repository = "library/" + out.repository;
    } else {
        out.registry = registry;
    }

    // 5. Default the tag only when the ref is not digest-pinned.
    if (out.tag.empty() && out.digest.empty())
        out.tag = kDefaultTag;

    return true;
}

} // namespace containers
