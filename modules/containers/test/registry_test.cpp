#include <gtest/gtest.h>

#include "registry.hpp"

using namespace containers;

// ── Bearer challenge ───────────────────────────────────────────────────────
TEST(Bearer, DockerHubChallenge) {
    const std::string hdr =
        "Www-Authenticate: Bearer realm=\"https://auth.docker.io/token\","
        "service=\"registry.docker.io\","
        "scope=\"repository:library/nginx:pull\"";
    auto c = parse_bearer_challenge(hdr);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.realm, "https://auth.docker.io/token");
    EXPECT_EQ(c.service, "registry.docker.io");
    EXPECT_EQ(c.scope, "repository:library/nginx:pull");
}

TEST(Bearer, NoScopeIsOk) {
    auto c = parse_bearer_challenge(
        "Bearer realm=\"https://ghcr.io/token\",service=\"ghcr.io\"");
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.realm, "https://ghcr.io/token");
    EXPECT_TRUE(c.scope.empty());
}

TEST(Bearer, BasicSchemeIsNotBearer) {
    auto c = parse_bearer_challenge("Www-Authenticate: Basic realm=\"x\"");
    EXPECT_FALSE(c.ok);
}

TEST(Bearer, TokenUrlBuild) {
    BearerChallenge c;
    c.realm = "https://auth.docker.io/token";
    c.service = "registry.docker.io";
    c.scope = "repository:library/nginx:pull";
    EXPECT_EQ(build_token_url(c),
              "https://auth.docker.io/token?service=registry.docker.io"
              "&scope=repository:library/nginx:pull");
}

TEST(Bearer, TokenUrlNoScope) {
    BearerChallenge c;
    c.realm = "https://ghcr.io/token";
    c.service = "ghcr.io";
    EXPECT_EQ(build_token_url(c), "https://ghcr.io/token?service=ghcr.io");
}

// ── Token response ─────────────────────────────────────────────────────────
TEST(Token, TokenField) {
    EXPECT_EQ(parse_token_response(R"({"token":"abc.def"})"), "abc.def");
}
TEST(Token, AccessTokenField) {
    EXPECT_EQ(parse_token_response(R"({"access_token":"xyz"})"), "xyz");
}
TEST(Token, GarbageIsEmpty) {
    EXPECT_EQ(parse_token_response("not json"), "");
}

// ── Manifest index / platform select ───────────────────────────────────────
static const char* kIndex = R"({
  "mediaType":"application/vnd.oci.image.index.v1+json",
  "manifests":[
    {"mediaType":"application/vnd.oci.image.manifest.v1+json",
     "digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111",
     "size":111,"platform":{"os":"linux","architecture":"amd64"}},
    {"mediaType":"application/vnd.oci.image.manifest.v1+json",
     "digest":"sha256:2222222222222222222222222222222222222222222222222222222222222222",
     "size":222,"platform":{"os":"linux","architecture":"arm64","variant":"v8"}},
    {"mediaType":"application/vnd.oci.image.manifest.v1+json",
     "digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333",
     "size":333,"platform":{"os":"unknown","architecture":"unknown"}}
  ]
})";

TEST(Index, IsIndex) {
    EXPECT_TRUE(manifest_is_index(kIndex));
    EXPECT_FALSE(manifest_is_index(R"({"config":{},"layers":[]})"));
}

TEST(Index, ParseDropsUnknown) {
    auto v = parse_manifest_index(kIndex);
    ASSERT_EQ(v.size(), 2u);          // unknown/unknown attestation dropped
    EXPECT_EQ(v[0].arch, "amd64");
    EXPECT_EQ(v[1].arch, "arm64");
    EXPECT_EQ(v[1].variant, "v8");
}

TEST(Index, SelectArm64FallsBackOverVariant) {
    auto v = parse_manifest_index(kIndex);
    // Want arm64 with empty variant; the entry is tagged v8 → fallback match.
    int i = select_platform(v, "linux", "arm64", "");
    ASSERT_GE(i, 0);
    EXPECT_EQ(v[i].digest,
        "sha256:2222222222222222222222222222222222222222222222222222222222222222");
}

TEST(Index, SelectExactAmd64) {
    auto v = parse_manifest_index(kIndex);
    int i = select_platform(v, "linux", "amd64", "");
    ASSERT_GE(i, 0);
    EXPECT_EQ(v[i].arch, "amd64");
}

TEST(Index, SelectMissingArchIsMinusOne) {
    auto v = parse_manifest_index(kIndex);
    EXPECT_EQ(select_platform(v, "linux", "riscv64", ""), -1);
}

// ── Image manifest ─────────────────────────────────────────────────────────
TEST(Manifest, ParseConfigAndLayers) {
    const char* m = R"({
      "config":{"mediaType":"application/vnd.oci.image.config.v1+json",
                "digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "size":7000},
      "layers":[
        {"mediaType":"application/vnd.oci.image.layer.v1.tar+gzip",
         "digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
         "size":100},
        {"mediaType":"application/vnd.oci.image.layer.v1.tar+gzip",
         "digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
         "size":200}
      ]
    })";
    auto im = parse_image_manifest(m);
    ASSERT_TRUE(im.ok);
    EXPECT_EQ(im.config.size, 7000);
    ASSERT_EQ(im.layers.size(), 2u);
    EXPECT_EQ(im.layers[1].size, 200);
}

TEST(Manifest, IndexIsNotImageManifest) {
    EXPECT_FALSE(parse_image_manifest(kIndex).ok);
}

// ── Digest validation + blob path (OTA \n-in-sha guard) ────────────────────
TEST(Digest, ValidLowercase64) {
    EXPECT_TRUE(is_valid_digest(
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
}
TEST(Digest, RejectsTrailingNewline) {
    EXPECT_FALSE(is_valid_digest(
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"));
}
TEST(Digest, RejectsUppercaseAndShort) {
    EXPECT_FALSE(is_valid_digest("sha256:ABCD"));
    EXPECT_FALSE(is_valid_digest("sha256:zz"));
    EXPECT_FALSE(is_valid_digest("md5:0123456789abcdef0123456789abcdef"));
}
TEST(Digest, BlobPath) {
    EXPECT_EQ(
        blob_path("/var/lib/iot-containers",
            "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        "/var/lib/iot-containers/blobs/sha256/"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    EXPECT_EQ(blob_path("/x", "bogus"), "");
}
