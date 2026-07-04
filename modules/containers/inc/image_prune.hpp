#ifndef __iot_container_image_prune_hpp__
#define __iot_container_image_prune_hpp__

#include <string>
#include <vector>

/// Pure planner for pruning dangling image content from the on-disk store.
///
/// The store is content-addressed: `manifests/<config-hex>.json`,
/// `blobs/sha256/<hex>` (config + layer tarballs) and `layers/<hex>/fs`
/// (extracted layers). When a container is re-pulled to a new image or removed,
/// the old image's manifest/blobs/layers become dangling. Because layers are
/// shared by digest, a layer may only be dropped when NO live image references
/// it — this planner computes exactly that set difference. Kept ACE/FS-free so
/// the shared-layer safety is host-unit-testable; the daemon does the manifest
/// reads, directory scan and deletes. See apps/docs/tdd-device-containers.md.

namespace containers {

/// The content one live (still-referenced) image keeps alive: its config blob
/// (== manifest key) and its layer blobs/extractions, all as bare sha256 hexes.
struct ImageRefs {
    std::string              config_hex;
    std::vector<std::string> layer_hexes;
};

/// What is physically on disk (bare hexes), from scanning the three store dirs.
struct DiskInventory {
    std::vector<std::string> manifests;   ///< manifests/<hex>.json
    std::vector<std::string> blobs;       ///< blobs/sha256/<hex>
    std::vector<std::string> layers;      ///< layers/<hex>/
};

/// Hexes to delete from each store dir.
struct PrunePlan {
    std::vector<std::string> manifests;
    std::vector<std::string> blobs;
    std::vector<std::string> layers;
};

/// Plan a prune: everything on disk not kept alive by a `live` image is dropped.
/// Orphan manifests (named by an image's config hex) are always safe to drop.
/// When `resolved_all` is false — some live image's manifest could not be read,
/// so its layer/blob set is unknown — blob and layer pruning is suppressed to
/// avoid deleting content that image still needs; only orphan manifests go.
PrunePlan plan_image_prune(const std::vector<ImageRefs>& live,
                           const DiskInventory& disk, bool resolved_all);

} // namespace containers

#endif /* __iot_container_image_prune_hpp__ */
