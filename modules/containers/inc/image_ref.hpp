#ifndef __iot_container_image_ref_hpp__
#define __iot_container_image_ref_hpp__

#include <string>

/// Pure OCI/Docker image-reference parsing — no ACE, no data-store, no libcurl,
/// so it is fully host-unit-testable. The registry v2 client (Phase 2) builds
/// its request URLs directly from the parsed fields. See
/// apps/docs/tdd-device-containers.md.

namespace containers {

/// Parsed image reference, normalized with Docker's rules.
///
///   registry   - registry host[:port] to contact. "docker.io" (and the bare /
///                single-segment form) is rewritten to the registry-1.docker.io
///                API endpoint.
///   repository - full repository path INCLUDING the implicit `library/`
///                namespace for single-segment Docker Hub images
///                (`nginx` -> `library/nginx`).
///   tag        - image tag; defaults to "latest" when neither a tag nor a
///                digest is given, and is empty when a digest pins the ref.
///   digest     - "sha256:..." when the ref is digest-pinned
///                (`repo@sha256:...`), otherwise empty.
struct ImageRef {
    std::string registry;
    std::string repository;
    std::string tag;
    std::string digest;
};

/// Canonical Docker Hub registry-v2 API host (what "docker.io" resolves to).
inline constexpr const char* kDockerRegistryHost = "registry-1.docker.io";

/// Normalize `ref` (e.g. "nginx", "nginx:1.25", "ghcr.io/owner/app:dev",
/// "localhost:5000/team/app@sha256:...") into `out`. Returns false when `ref`
/// is empty or malformed (empty repository, trailing ':' / '@'); `out` is left
/// unspecified on false.
bool parse_image_ref(const std::string& ref, ImageRef& out);

} // namespace containers

#endif /* __iot_container_image_ref_hpp__ */
