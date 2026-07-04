#include "image_prune.hpp"

#include <set>

namespace containers {

namespace {
std::vector<std::string> minus(const std::vector<std::string>& on_disk,
                               const std::set<std::string>& keep) {
    std::vector<std::string> out;
    for (const auto& h : on_disk)
        if (!keep.count(h)) out.push_back(h);
    return out;
}
} // namespace

PrunePlan plan_image_prune(const std::vector<ImageRefs>& live,
                           const DiskInventory& disk, bool resolved_all) {
    std::set<std::string> keep_manifests, keep_blobs, keep_layers;
    for (const auto& im : live) {
        if (!im.config_hex.empty()) {
            keep_manifests.insert(im.config_hex);
            keep_blobs.insert(im.config_hex);          // the config blob
        }
        for (const auto& l : im.layer_hexes) {
            keep_blobs.insert(l);                      // the layer tarball
            keep_layers.insert(l);                     // its extraction
        }
    }

    PrunePlan plan;
    // Orphan manifests are always safe: a manifest is named by its own image's
    // config hex, so one not backing a live image is dangling regardless.
    plan.manifests = minus(disk.manifests, keep_manifests);
    // Blob/layer content is only pruned when every live image resolved — else a
    // live image whose layer set we couldn't read might own some of it.
    if (resolved_all) {
        plan.blobs  = minus(disk.blobs,  keep_blobs);
        plan.layers = minus(disk.layers, keep_layers);
    }
    return plan;
}

} // namespace containers
