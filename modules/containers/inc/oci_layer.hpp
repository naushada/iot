#ifndef __iot_container_oci_layer_hpp__
#define __iot_container_oci_layer_hpp__

#include <string>
#include <vector>

/// Pure OCI image-layer helpers — tar-header parsing, OCI-whiteout
/// classification, path-traversal safety, and overlayfs lowerdir ordering. No
/// zlib, no filesystem, no ACE, so it is fully host-unit-testable. The actual
/// gzip/tar extraction + mknod/symlink/xattr application + the overlay mount
/// live in the daemon's layer_store. See apps/docs/tdd-device-containers.md.

namespace containers {

// ── tar (ustar / GNU) ──────────────────────────────────────────────────────
namespace tar {
constexpr int kRegular   = '0';
constexpr int kRegularA  = '\0';   // legacy regular-file typeflag
constexpr int kHardLink  = '1';
constexpr int kSymlink   = '2';
constexpr int kCharDev   = '3';
constexpr int kBlockDev  = '4';
constexpr int kDirectory = '5';
constexpr int kFifo      = '6';
constexpr int kGnuLongName = 'L';  // entry data = name of the FOLLOWING entry
constexpr int kGnuLongLink = 'K';  // entry data = linkname of the following entry
constexpr int kPaxExtended = 'x';  // PAX per-file extended header (key=value)
constexpr int kPaxGlobal   = 'g';  // PAX global header (ignored)
constexpr int kBlockSize  = 512;
}

struct TarEntry {
    std::string name;        ///< full path (prefix + name), as stored
    std::string linkname;    ///< link target for sym/hard links
    long long   size = 0;    ///< file data size in bytes
    unsigned    mode = 0;    ///< permission bits (octal-decoded)
    unsigned    uid  = 0;    ///< owner uid (for chown of the extracted file)
    unsigned    gid  = 0;    ///< owner gid
    int         type = tar::kRegularA;
};

/// Parse a 512-byte tar header block. Sets `is_zero_block` when the block is
/// all-zero (archive end marker) — in that case the return value is false and
/// `out` is untouched. Returns false (with is_zero_block=false) on a malformed
/// header. ustar `prefix` is joined to `name` with '/'.
bool parse_tar_header(const char block[tar::kBlockSize], TarEntry& out,
                      bool& is_zero_block);

/// Decode a tar octal numeric field (`len` bytes, space/NUL terminated).
/// Returns false on a non-octal byte. Leading spaces are skipped; an empty
/// field decodes to 0.
bool parse_tar_octal(const char* field, std::size_t len, long long& out);

// ── OCI whiteouts ──────────────────────────────────────────────────────────
enum class WhiteoutKind {
    None,      ///< a normal entry
    Remove,    ///< `<dir>/.wh.<name>` — `<dir>/<name>` is deleted from lowers
    Opaque,    ///< `<dir>/.wh..wh..opq` — `<dir>` hides all lower contents
};

/// Classify an entry path as an OCI whiteout. For Remove/Opaque, `target_rel`
/// is set to the affected path (the file to whiteout, or the dir to mark
/// opaque) relative to the layer root.
WhiteoutKind classify_whiteout(const std::string& path, std::string& target_rel);

// ── Path safety + overlay ──────────────────────────────────────────────────
/// Normalize a tar member name into a safe relative path: strips a leading
/// "./" and "/", collapses "." components, and REJECTS any ".." component or
/// otherwise-escaping path (returns "" — caller must skip the entry). Prevents
/// a malicious layer from writing outside its extraction dir.
std::string safe_rel_path(const std::string& name);

/// Build the overlayfs `lowerdir=` value from image layer dirs ordered
/// base→top (OCI order). overlayfs wants highest-priority (top) first, so the
/// list is reversed and ':'-joined.
std::string overlay_lowerdir(const std::vector<std::string>& layer_dirs_base_to_top);

} // namespace containers

#endif /* __iot_container_oci_layer_hpp__ */
