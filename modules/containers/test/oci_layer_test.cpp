#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "oci_layer.hpp"

using namespace containers;

namespace {

// Build a minimal ustar header block for tests.
std::string make_header(const std::string& name, long long size, char type,
                        const std::string& linkname = "",
                        const std::string& prefix = "",
                        unsigned mode = 0644) {
    std::string b(tar::kBlockSize, '\0');
    auto put = [&](std::size_t off, const std::string& s, std::size_t max) {
        std::memcpy(&b[off], s.data(), std::min(s.size(), max));
    };
    auto put_octal = [&](std::size_t off, long long v, std::size_t width) {
        // width-1 octal digits, NUL-terminated (classic tar field).
        char buf[32];
        std::snprintf(buf, sizeof buf, "%0*llo", static_cast<int>(width - 1), v);
        put(off, buf, width - 1);
    };
    put(0, name, 100);
    put_octal(100, mode, 8);
    put_octal(124, size, 12);
    b[156] = type;
    put(157, linkname, 100);
    put(257, "ustar", 6);
    put(263, "00", 2);
    if (!prefix.empty()) put(345, prefix, 155);
    return b;
}

} // namespace

// ── octal ──────────────────────────────────────────────────────────────────
TEST(TarOctal, Basic) {
    long long v = -1;
    ASSERT_TRUE(parse_tar_octal("000644 \0", 8, v));
    EXPECT_EQ(v, 0644);
}
TEST(TarOctal, EmptyIsZero) {
    long long v = -1;
    ASSERT_TRUE(parse_tar_octal("        ", 8, v));
    EXPECT_EQ(v, 0);
}
TEST(TarOctal, RejectsNonOctal) {
    long long v = 0;
    EXPECT_FALSE(parse_tar_octal("12389\0\0\0", 8, v));
}

// ── header ───────────────────────────────────────────────────────────────
TEST(TarHeader, RegularFile) {
    auto h = make_header("etc/hostname", 5, tar::kRegular);
    TarEntry e; bool zero = false;
    ASSERT_TRUE(parse_tar_header(h.data(), e, zero));
    EXPECT_FALSE(zero);
    EXPECT_EQ(e.name, "etc/hostname");
    EXPECT_EQ(e.size, 5);
    EXPECT_EQ(e.type, tar::kRegular);
    EXPECT_EQ(e.mode, 0644u);
}
TEST(TarHeader, UstarPrefixJoined) {
    auto h = make_header("hostname", 0, tar::kRegular, "", "very/long/etc");
    TarEntry e; bool zero = false;
    ASSERT_TRUE(parse_tar_header(h.data(), e, zero));
    EXPECT_EQ(e.name, "very/long/etc/hostname");
}
TEST(TarHeader, Symlink) {
    auto h = make_header("bin/sh", 0, tar::kSymlink, "busybox");
    TarEntry e; bool zero = false;
    ASSERT_TRUE(parse_tar_header(h.data(), e, zero));
    EXPECT_EQ(e.type, tar::kSymlink);
    EXPECT_EQ(e.linkname, "busybox");
}
TEST(TarHeader, ZeroBlockIsEnd) {
    std::string b(tar::kBlockSize, '\0');
    TarEntry e; bool zero = false;
    EXPECT_FALSE(parse_tar_header(b.data(), e, zero));
    EXPECT_TRUE(zero);
}

// ── whiteouts ──────────────────────────────────────────────────────────────
TEST(Whiteout, RemoveFile) {
    std::string target;
    EXPECT_EQ(classify_whiteout("usr/bin/.wh.oldtool", target), WhiteoutKind::Remove);
    EXPECT_EQ(target, "usr/bin/oldtool");
}
TEST(Whiteout, RemoveAtRoot) {
    std::string target;
    EXPECT_EQ(classify_whiteout(".wh.toplevel", target), WhiteoutKind::Remove);
    EXPECT_EQ(target, "toplevel");
}
TEST(Whiteout, OpaqueDir) {
    std::string target;
    EXPECT_EQ(classify_whiteout("var/cache/.wh..wh..opq", target), WhiteoutKind::Opaque);
    EXPECT_EQ(target, "var/cache");
}
TEST(Whiteout, NormalIsNone) {
    std::string target;
    EXPECT_EQ(classify_whiteout("etc/passwd", target), WhiteoutKind::None);
    // ".whatever" is NOT a whiteout — the prefix is exactly ".wh." (the 4th
    // char must be a dot), so this is a normal dotfile.
    EXPECT_EQ(classify_whiteout("etc/.whatever", target), WhiteoutKind::None);
}

// ── path safety ────────────────────────────────────────────────────────────
TEST(SafePath, StripsLeadingDotSlash) {
    EXPECT_EQ(safe_rel_path("./etc/hostname"), "etc/hostname");
    EXPECT_EQ(safe_rel_path("/etc/hostname"), "etc/hostname");
}
TEST(SafePath, RejectsTraversal) {
    EXPECT_EQ(safe_rel_path("../etc/shadow"), "");
    EXPECT_EQ(safe_rel_path("a/../../b"), "");
    EXPECT_EQ(safe_rel_path("a/b/../c"), "");   // any ".." rejected (conservative)
}
TEST(SafePath, RootIsEmpty) {
    EXPECT_EQ(safe_rel_path("."), "");
    EXPECT_EQ(safe_rel_path("./"), "");
}

// ── overlay lowerdir ordering ──────────────────────────────────────────────
TEST(Overlay, TopLayerFirst) {
    // OCI order is base → top; overlay wants top first.
    std::vector<std::string> dirs = {"/l/base", "/l/mid", "/l/top"};
    EXPECT_EQ(overlay_lowerdir(dirs), "/l/top:/l/mid:/l/base");
}
TEST(Overlay, SingleLayer) {
    EXPECT_EQ(overlay_lowerdir({"/l/only"}), "/l/only");
}
TEST(Overlay, DedupesRepeatedLayerPaths) {
    // An image can list the same layer digest twice → same extracted dir.
    // Linux 6.6 overlay rejects a duplicate lowerdir ("conflicting lowerdir
    // path"), so we keep the topmost occurrence only.
    std::vector<std::string> dirs = {"/l/base", "/l/dup", "/l/mid", "/l/dup"};
    EXPECT_EQ(overlay_lowerdir(dirs), "/l/dup:/l/mid:/l/base");
}
TEST(Overlay, AllIdenticalCollapseToOne) {
    EXPECT_EQ(overlay_lowerdir({"/l/x", "/l/x", "/l/x"}), "/l/x");
}
