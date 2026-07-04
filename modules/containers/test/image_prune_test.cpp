#include <gtest/gtest.h>
#include <algorithm>

#include "image_prune.hpp"

using namespace containers;

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Two live images share a base layer "L0"; a third, dangling image owned "cfgD"
// + a unique layer "LD". Prune drops the dangling image's manifest/config/layer
// but MUST keep the shared base layer.
TEST(ImagePrune, DropsDanglingKeepsSharedLayer) {
    std::vector<ImageRefs> live = {
        {"cfgA", {"L0", "L1"}},
        {"cfgB", {"L0", "L2"}},
    };
    DiskInventory disk;
    disk.manifests = {"cfgA", "cfgB", "cfgD"};
    disk.blobs     = {"cfgA", "cfgB", "cfgD", "L0", "L1", "L2", "LD"};
    disk.layers    = {"L0", "L1", "L2", "LD"};

    PrunePlan p = plan_image_prune(live, disk, /*resolved_all=*/true);

    EXPECT_EQ(p.manifests, std::vector<std::string>{"cfgD"});
    EXPECT_TRUE(has(p.blobs, "cfgD"));
    EXPECT_TRUE(has(p.blobs, "LD"));
    EXPECT_TRUE(has(p.layers, "LD"));
    // Shared + live content survives.
    EXPECT_FALSE(has(p.blobs, "L0"));
    EXPECT_FALSE(has(p.layers, "L0"));
    EXPECT_FALSE(has(p.blobs, "cfgA"));
}

TEST(ImagePrune, NothingLiveDropsEverything) {
    DiskInventory disk;
    disk.manifests = {"c1"};
    disk.blobs     = {"c1", "l1"};
    disk.layers    = {"l1"};
    PrunePlan p = plan_image_prune({}, disk, true);
    EXPECT_EQ(p.manifests.size(), 1u);
    EXPECT_EQ(p.blobs.size(), 2u);
    EXPECT_EQ(p.layers.size(), 1u);
}

TEST(ImagePrune, NothingDanglingIsNoop) {
    std::vector<ImageRefs> live = {{"cfgA", {"L0"}}};
    DiskInventory disk;
    disk.manifests = {"cfgA"};
    disk.blobs     = {"cfgA", "L0"};
    disk.layers    = {"L0"};
    PrunePlan p = plan_image_prune(live, disk, true);
    EXPECT_TRUE(p.manifests.empty());
    EXPECT_TRUE(p.blobs.empty());
    EXPECT_TRUE(p.layers.empty());
}

// When a live image's manifest couldn't be read (resolved_all=false), its layer
// set is unknown — suppress blob/layer pruning so we never delete content it may
// own; still safe to remove orphan manifests.
TEST(ImagePrune, ConservativeWhenUnresolved) {
    std::vector<ImageRefs> live = {{"cfgA", {}}};   // layers unknown
    DiskInventory disk;
    disk.manifests = {"cfgA", "cfgOrphan"};
    disk.blobs     = {"cfgA", "L0", "L1"};
    disk.layers    = {"L0", "L1"};
    PrunePlan p = plan_image_prune(live, disk, /*resolved_all=*/false);
    EXPECT_EQ(p.manifests, std::vector<std::string>{"cfgOrphan"});
    EXPECT_TRUE(p.blobs.empty());     // suppressed
    EXPECT_TRUE(p.layers.empty());    // suppressed
}
