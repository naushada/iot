#ifndef __iot_container_layer_store_hpp__
#define __iot_container_layer_store_hpp__

#include <string>
#include <vector>

/// Layer store: gzip+tar layer extraction (with OCI-whiteout conversion) into a
/// content-addressed cache, and the overlayfs mount that assembles them into a
/// container rootfs. Uses zlib + filesystem syscalls + mount(2), so it lives in
/// the daemon (the pure tar/whiteout/overlay-order logic is in oci_layer).
/// See apps/docs/tdd-device-containers.md.

namespace containers {

struct MountResult {
    bool        ok = false;
    std::string merged;     ///< merged rootfs path (run_root/<id>/rootfs)
    std::string error;
};

/// Extract a single gzip-compressed tar layer `blob_file` into `dest_dir`
/// (created if needed), converting OCI whiteouts to overlayfs whiteouts
/// (`.wh.<n>` → char dev 0:0; `.wh..wh..opq` → trusted.overlay.opaque xattr).
/// Must run as root (mknod, chown, trusted xattr). False with `err` on failure.
bool extract_layer(const std::string& blob_file, const std::string& dest_dir,
                   std::string& err);

/// Ensure every layer digest in `layer_digests` (OCI order: base→top) is
/// extracted under `<root>/layers/<hex>/fs` (cached via a `.done` marker so a
/// re-run is instant). Reads blobs from `<root>/blobs/sha256/<hex>`. Returns
/// the ordered extracted `fs` dirs in `out_dirs`. False with `err` on failure.
bool ensure_layers_extracted(const std::string&              root,
                             const std::vector<std::string>& layer_digests,
                             std::vector<std::string>&       out_dirs,
                             std::string&                    err);

/// Mount the overlay for container `id`: lowerdir from `layer_dirs` (base→top),
/// with upper/work/merged created under `<run_root>/<id>/`. Any stale mount for
/// `id` is torn down first. Must run as root (mount).
MountResult mount_overlay(const std::vector<std::string>& layer_dirs,
                          const std::string&              run_root,
                          const std::string&              id);

/// Unmount the overlay for container `id` and remove its run dir. Idempotent
/// (no-op when not mounted). False with `err` only on a hard failure.
bool unmount_overlay(const std::string& run_root, const std::string& id,
                     std::string& err);

} // namespace containers

#endif /* __iot_container_layer_store_hpp__ */
