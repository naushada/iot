#include "oci_layer.hpp"

#include <sstream>
#include <unordered_set>

namespace containers {

namespace {

// Read a fixed-width tar field up to the first NUL (or the full width).
std::string field_str(const char* p, std::size_t maxlen) {
    std::size_t n = 0;
    while (n < maxlen && p[n] != '\0') ++n;
    return std::string(p, n);
}

std::string dirname_of(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string basename_of(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool parse_tar_octal(const char* field, std::size_t len, long long& out) {
    std::size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;   // leading pad
    long long v = 0;
    bool any = false;
    for (; i < len; ++i) {
        char c = field[i];
        if (c == ' ' || c == '\0') break;                            // trailing pad
        if (c < '0' || c > '7') return false;
        v = (v << 3) + (c - '0');
        any = true;
    }
    out = any ? v : 0;
    return true;
}

bool parse_tar_header(const char block[tar::kBlockSize], TarEntry& out,
                      bool& is_zero_block) {
    is_zero_block = false;
    bool all_zero = true;
    for (int i = 0; i < tar::kBlockSize; ++i) {
        if (block[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) { is_zero_block = true; return false; }

    long long mode = 0, size = 0, uid = 0, gid = 0;
    if (!parse_tar_octal(block + 100, 8, mode)) return false;
    if (!parse_tar_octal(block + 124, 12, size)) return false;
    // uid/gid are best-effort: a bad field shouldn't reject the whole header.
    parse_tar_octal(block + 108, 8, uid);
    parse_tar_octal(block + 116, 8, gid);

    out = TarEntry{};
    std::string name = field_str(block, 100);
    // ustar: a non-empty `prefix` is joined to `name` with '/'.
    const std::string magic = field_str(block + 257, 6);
    if (magic.rfind("ustar", 0) == 0) {
        const std::string prefix = field_str(block + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
    }
    out.name     = name;
    out.linkname = field_str(block + 157, 100);
    out.size     = size;
    out.mode     = static_cast<unsigned>(mode) & 07777;
    out.uid      = static_cast<unsigned>(uid);
    out.gid      = static_cast<unsigned>(gid);
    const char tf = block[156];
    out.type = (tf == '\0') ? tar::kRegularA : tf;
    return true;
}

WhiteoutKind classify_whiteout(const std::string& path, std::string& target_rel) {
    const std::string base = basename_of(path);
    const std::string dir  = dirname_of(path);
    static const std::string opq = ".wh..wh..opq";
    static const std::string pre = ".wh.";

    if (base == opq) {
        target_rel = dir;                 // may be "" → the layer root itself
        return WhiteoutKind::Opaque;
    }
    if (base.size() > pre.size() && base.compare(0, pre.size(), pre) == 0) {
        const std::string real = base.substr(pre.size());
        target_rel = dir.empty() ? real : dir + "/" + real;
        return WhiteoutKind::Remove;
    }
    target_rel.clear();
    return WhiteoutKind::None;
}

std::string safe_rel_path(const std::string& name) {
    std::vector<std::string> out;
    std::string comp;
    std::stringstream ss(name);
    while (std::getline(ss, comp, '/')) {
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return {};      // traversal — reject the whole path
        out.push_back(comp);
    }
    std::string joined;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) joined += '/';
        joined += out[i];
    }
    return joined;                        // "" when the name was "."/"./"/empty
}

std::string overlay_lowerdir(const std::vector<std::string>& layer_dirs_base_to_top) {
    std::string out;
    // overlayfs wants the highest-priority (topmost) layer first.
    //
    // Dedupe repeated paths. An OCI image can list the same layer digest more
    // than once (e.g. the shared empty layer, or genuinely identical layers);
    // each maps to the SAME extracted dir, so a naive join yields
    // `lowerdir=/x/fs:/x/fs`. Linux 6.6's rewritten overlay mount parser
    // REJECTS a duplicate lowerdir with "overlayfs: conflicting lowerdir path"
    // (older kernels silently tolerated it — why this only bit on HW). Identical
    // digest == identical content, so keeping the topmost occurrence preserves
    // the merged view.
    std::unordered_set<std::string> seen;
    for (auto it = layer_dirs_base_to_top.rbegin(); it != layer_dirs_base_to_top.rend(); ++it) {
        if (!seen.insert(*it).second) continue;   // already emitted this path
        if (!out.empty()) out += ':';
        out += *it;
    }
    return out;
}

} // namespace containers
