#include "layer_store.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <zlib.h>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>

#include "http_client.hpp"   // mkdir_p
#include "oci_layer.hpp"
#include "registry.hpp"      // is_valid_digest / blob_path

namespace containers {

namespace {

std::string dirname_of(const std::string& p) {
    auto s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

bool ensure_parent(const std::string& full) {
    const std::string d = dirname_of(full);
    return d.empty() ? true : mkdir_p(d);
}

// Read exactly `n` decompressed bytes. Returns bytes read (n on success, 0 on
// clean EOF, <n on truncation).
int gz_read_full(gzFile gz, char* buf, int n) {
    int total = 0;
    while (total < n) {
        int r = gzread(gz, buf + total, n - total);
        if (r <= 0) break;
        total += r;
    }
    return total;
}

// Skip the 512-byte zero padding after `size` bytes of entry data.
bool gz_skip(gzFile gz, long long count) {
    char buf[8192];
    while (count > 0) {
        int want = static_cast<int>(count > (long long)sizeof buf ? sizeof buf : count);
        int r = gzread(gz, buf, want);
        if (r <= 0) return false;
        count -= r;
    }
    return true;
}

bool gz_skip_data(gzFile gz, long long size) {
    long long pad = (512 - (size % 512)) % 512;
    return gz_skip(gz, size + pad);
}

bool gz_read_data_str(gzFile gz, long long size, std::string& out) {
    out.clear();
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && gz_read_full(gz, &out[0], static_cast<int>(size)) != size) return false;
    long long pad = (512 - (size % 512)) % 512;
    return gz_skip(gz, pad);
}

// Stream `size` data bytes from gz into fd, then consume the padding.
bool gz_stream_to_fd(gzFile gz, int fd, long long size) {
    char buf[65536];
    long long remaining = size;
    while (remaining > 0) {
        int want = static_cast<int>(remaining > (long long)sizeof buf ? sizeof buf : remaining);
        int r = gzread(gz, buf, want);
        if (r <= 0) return false;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = ::write(fd, buf + off, r - off);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            off += w;
        }
        remaining -= r;
    }
    long long pad = (512 - (size % 512)) % 512;
    return gz_skip(gz, pad);
}

// Parse PAX extended-header records ("<len> key=value\n"...) for path/linkpath.
void parse_pax(const std::string& s, std::string& path, std::string& linkpath) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i) break;
        long len = std::strtol(s.substr(i, j - i).c_str(), nullptr, 10);
        if (len <= 0 || i + static_cast<std::size_t>(len) > s.size()) break;
        std::string rec = s.substr(j + 1, i + len - (j + 1));   // "key=value\n"
        if (!rec.empty() && rec.back() == '\n') rec.pop_back();
        auto eq = rec.find('=');
        if (eq != std::string::npos) {
            const std::string k = rec.substr(0, eq), v = rec.substr(eq + 1);
            if (k == "path") path = v;
            else if (k == "linkpath") linkpath = v;
        }
        i += static_cast<std::size_t>(len);
    }
}

int nftw_rm(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }

void rm_rf(const std::string& path) {
    if (ACE_OS::access(path.c_str(), F_OK) != 0) return;
    ::nftw(path.c_str(), nftw_rm, 16, FTW_DEPTH | FTW_PHYS);
}

// Apply a single (non long-name/PAX) tar entry under dest_dir. Sets
// `streamed` when it consumed the entry's data (a regular file) so the caller
// does not skip it again.
void apply_entry(const std::string& dest_dir, const std::string& rel,
                 const TarEntry& e, const std::string& link, gzFile gz,
                 bool& streamed) {
    streamed = false;

    std::string target;
    const WhiteoutKind wk = classify_whiteout(rel, target);
    if (wk == WhiteoutKind::Remove) {
        const std::string tp = dest_dir + "/" + target;
        ensure_parent(tp);
        ::unlink(tp.c_str());
        if (::mknod(tp.c_str(), S_IFCHR | 0, makedev(0, 0)) != 0)
            ACE_DEBUG((LM_WARNING, ACE_TEXT("%D [ctr] whiteout mknod %C: %C\n"),
                       tp.c_str(), std::strerror(errno)));
        return;
    }
    if (wk == WhiteoutKind::Opaque) {
        const std::string tp = target.empty() ? dest_dir : dest_dir + "/" + target;
        mkdir_p(tp);
        if (::setxattr(tp.c_str(), "trusted.overlay.opaque", "y", 1, 0) != 0)
            ACE_DEBUG((LM_WARNING, ACE_TEXT("%D [ctr] opaque xattr %C: %C\n"),
                       tp.c_str(), std::strerror(errno)));
        return;
    }

    const std::string full = dest_dir + "/" + rel;
    switch (e.type) {
        case tar::kDirectory:
            mkdir_p(full);
            ::chmod(full.c_str(), e.mode ? e.mode : 0755);
            ::chown(full.c_str(), e.uid, e.gid);
            break;
        case tar::kRegular:
        case tar::kRegularA: {
            ensure_parent(full);
            ::unlink(full.c_str());
            int fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                            e.mode ? e.mode : 0644);
            if (fd < 0) {
                ACE_DEBUG((LM_WARNING, ACE_TEXT("%D [ctr] open %C: %C\n"),
                           full.c_str(), std::strerror(errno)));
                break;   // streamed stays false → caller skips the data
            }
            if (!gz_stream_to_fd(gz, fd, e.size))
                ACE_DEBUG((LM_WARNING, ACE_TEXT("%D [ctr] short write %C\n"), full.c_str()));
            ::fchmod(fd, e.mode ? e.mode : 0644);
            ::fchown(fd, e.uid, e.gid);
            ::close(fd);
            streamed = true;
            break;
        }
        case tar::kSymlink:
            ensure_parent(full);
            ::unlink(full.c_str());
            if (::symlink(link.c_str(), full.c_str()) == 0)
                ::lchown(full.c_str(), e.uid, e.gid);
            break;
        case tar::kHardLink: {
            ensure_parent(full);
            const std::string lrel = safe_rel_path(link);
            if (!lrel.empty()) {
                ::unlink(full.c_str());
                ::link((dest_dir + "/" + lrel).c_str(), full.c_str());
            }
            break;
        }
        default:
            // char/block/fifo device nodes in a layer are skipped: the runtime
            // populates /dev. (Whiteouts above already handled the 0/0 case.)
            break;
    }
}

} // namespace

bool extract_layer(const std::string& blob_file, const std::string& dest_dir,
                   std::string& err) {
    if (!mkdir_p(dest_dir)) { err = "could not create " + dest_dir; return false; }
    gzFile gz = gzopen(blob_file.c_str(), "rb");
    if (!gz) { err = "gzopen failed: " + blob_file; return false; }

    std::string pending_name, pending_link;
    char block[tar::kBlockSize];
    bool ok = true;

    while (true) {
        int n = gz_read_full(gz, block, tar::kBlockSize);
        if (n == 0) break;                                 // clean EOF
        if (n != tar::kBlockSize) { err = "truncated tar"; ok = false; break; }

        TarEntry e;
        bool zero = false;
        if (!parse_tar_header(block, e, zero)) {
            if (zero) break;                               // end-of-archive marker
            err = "malformed tar header"; ok = false; break;
        }

        if (e.type == tar::kGnuLongName || e.type == tar::kGnuLongLink) {
            std::string data;
            if (!gz_read_data_str(gz, e.size, data)) { err = "truncated long name"; ok = false; break; }
            if (!data.empty() && data.back() == '\0') data.pop_back();
            (e.type == tar::kGnuLongName ? pending_name : pending_link) = data;
            continue;
        }
        if (e.type == tar::kPaxExtended) {
            std::string data;
            if (!gz_read_data_str(gz, e.size, data)) { err = "truncated pax"; ok = false; break; }
            parse_pax(data, pending_name, pending_link);
            continue;
        }
        if (e.type == tar::kPaxGlobal) {
            if (!gz_skip_data(gz, e.size)) { err = "truncated pax-global"; ok = false; break; }
            continue;
        }

        const std::string name = pending_name.empty() ? e.name : pending_name;
        const std::string link = pending_link.empty() ? e.linkname : pending_link;
        pending_name.clear();
        pending_link.clear();

        const std::string rel = safe_rel_path(name);
        bool streamed = false;
        if (!rel.empty()) apply_entry(dest_dir, rel, e, link, gz, streamed);
        if (!streamed) {
            if (!gz_skip_data(gz, e.size)) { err = "truncated entry data"; ok = false; break; }
        }
    }

    gzclose(gz);
    return ok;
}

bool ensure_layers_extracted(const std::string&              root,
                             const std::vector<std::string>& layer_digests,
                             std::vector<std::string>&       out_dirs,
                             std::string&                    err) {
    out_dirs.clear();
    for (const auto& digest : layer_digests) {
        if (!is_valid_digest(digest)) { err = "layer has a malformed digest"; return false; }
        const std::string hex   = digest.substr(7);
        const std::string base  = root + "/layers/" + hex;
        const std::string fsdir = base + "/fs";
        const std::string done  = base + "/.done";

        if (ACE_OS::access(done.c_str(), F_OK) == 0) {     // cache hit
            out_dirs.push_back(fsdir);
            continue;
        }

        const std::string blob = blob_path(root, digest);
        if (blob.empty() || ACE_OS::access(blob.c_str(), F_OK) != 0) {
            err = "missing blob for layer " + digest; return false;
        }
        // Fresh extraction: clear any partial dir first.
        rm_rf(fsdir);
        if (!extract_layer(blob, fsdir, err)) return false;

        FILE* mk = std::fopen(done.c_str(), "w");
        if (mk) std::fclose(mk);
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] extracted layer %C\n"), hex.c_str()));
        out_dirs.push_back(fsdir);
    }
    return true;
}

MountResult mount_overlay(const std::vector<std::string>& layer_dirs,
                          const std::string&              run_root,
                          const std::string&              id) {
    MountResult res;
    if (layer_dirs.empty()) { res.error = "image has no layers"; return res; }

    const std::string idd    = run_root + "/" + id;
    const std::string upper  = idd + "/upper";
    const std::string work   = idd + "/work";
    const std::string merged = idd + "/rootfs";

    std::string e;
    unmount_overlay(run_root, id, e);                      // clear any stale mount

    if (!mkdir_p(upper) || !mkdir_p(work) || !mkdir_p(merged)) {
        res.error = "could not create overlay dirs under " + idd;
        return res;
    }

    const std::string opts = "lowerdir=" + overlay_lowerdir(layer_dirs) +
                             ",upperdir=" + upper + ",workdir=" + work;
    if (::mount("overlay", merged.c_str(), "overlay", 0, opts.c_str()) != 0) {
        res.error = std::string("overlay mount failed: ") + std::strerror(errno);
        return res;
    }
    res.merged = merged;
    res.ok = true;
    return res;
}

bool unmount_overlay(const std::string& run_root, const std::string& id,
                     std::string& err) {
    (void) err;
    const std::string idd    = run_root + "/" + id;
    const std::string merged = idd + "/rootfs";
    if (ACE_OS::access(merged.c_str(), F_OK) == 0) {
        if (::umount2(merged.c_str(), 0) != 0 && errno != EINVAL && errno != ENOENT)
            ::umount2(merged.c_str(), MNT_DETACH);          // lazy fallback
    }
    rm_rf(idd);
    return true;
}

} // namespace containers
