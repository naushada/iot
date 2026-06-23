#include <gtest/gtest.h>

#include "image_ref.hpp"

using containers::ImageRef;
using containers::parse_image_ref;

TEST(ImageRef, BareNameDefaultsToDockerHubLibraryLatest) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("nginx", r));
    EXPECT_EQ(r.registry, "registry-1.docker.io");
    EXPECT_EQ(r.repository, "library/nginx");
    EXPECT_EQ(r.tag, "latest");
    EXPECT_TRUE(r.digest.empty());
}

TEST(ImageRef, NameWithTag) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("nginx:1.25", r));
    EXPECT_EQ(r.registry, "registry-1.docker.io");
    EXPECT_EQ(r.repository, "library/nginx");
    EXPECT_EQ(r.tag, "1.25");
}

TEST(ImageRef, DockerHubOrgRepoKeepsBothSegments) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("grafana/grafana:10.0.0", r));
    EXPECT_EQ(r.registry, "registry-1.docker.io");
    EXPECT_EQ(r.repository, "grafana/grafana");   // no implicit library/
    EXPECT_EQ(r.tag, "10.0.0");
}

TEST(ImageRef, ExplicitDockerIoIsRewritten) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("docker.io/library/nginx:latest", r));
    EXPECT_EQ(r.registry, "registry-1.docker.io");
    EXPECT_EQ(r.repository, "library/nginx");
    EXPECT_EQ(r.tag, "latest");
}

TEST(ImageRef, ThirdPartyRegistry) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("ghcr.io/owner/app:dev", r));
    EXPECT_EQ(r.registry, "ghcr.io");
    EXPECT_EQ(r.repository, "owner/app");
    EXPECT_EQ(r.tag, "dev");
}

TEST(ImageRef, RegistryWithPortIsNotMistakenForTag) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("localhost:5000/team/app:1", r));
    EXPECT_EQ(r.registry, "localhost:5000");
    EXPECT_EQ(r.repository, "team/app");
    EXPECT_EQ(r.tag, "1");
}

TEST(ImageRef, RegistryWithPortAndNoTagDefaultsLatest) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("localhost:5000/app", r));
    EXPECT_EQ(r.registry, "localhost:5000");
    EXPECT_EQ(r.repository, "app");
    EXPECT_EQ(r.tag, "latest");
}

TEST(ImageRef, DigestPinHasNoDefaultTag) {
    const std::string ref =
        "nginx@sha256:0000000000000000000000000000000000000000000000000000000000000000";
    ImageRef r;
    ASSERT_TRUE(parse_image_ref(ref, r));
    EXPECT_EQ(r.repository, "library/nginx");
    EXPECT_TRUE(r.tag.empty());
    EXPECT_EQ(r.digest,
              "sha256:0000000000000000000000000000000000000000000000000000000000000000");
}

TEST(ImageRef, RegistryRepoDigestPin) {
    ImageRef r;
    ASSERT_TRUE(parse_image_ref("ghcr.io/owner/app@sha256:abc", r));
    EXPECT_EQ(r.registry, "ghcr.io");
    EXPECT_EQ(r.repository, "owner/app");
    EXPECT_TRUE(r.tag.empty());
    EXPECT_EQ(r.digest, "sha256:abc");
}

TEST(ImageRef, EmptyIsRejected) {
    ImageRef r;
    EXPECT_FALSE(parse_image_ref("", r));
}

TEST(ImageRef, TrailingColonIsRejected) {
    ImageRef r;
    EXPECT_FALSE(parse_image_ref("nginx:", r));
}

TEST(ImageRef, TrailingAtIsRejected) {
    ImageRef r;
    EXPECT_FALSE(parse_image_ref("nginx@", r));
}
