#ifndef __iot_container_registry_hpp__
#define __iot_container_registry_hpp__

#include <string>
#include <vector>

/// Pure OCI/Docker registry-v2 protocol helpers — the string/JSON logic with
/// NO network and NO ACE, so it is fully host-unit-testable. The actual HTTP
/// transfers (curl) live in the daemon's http_client; registry_puller stitches
/// these helpers to it. See apps/docs/tdd-device-containers.md.

namespace containers {

// ── Manifest media types (Accept negotiation) ──────────────────────────────
inline constexpr const char* kMediaOciIndex   = "application/vnd.oci.image.index.v1+json";
inline constexpr const char* kMediaOciManifest = "application/vnd.oci.image.manifest.v1+json";
inline constexpr const char* kMediaDockerList  = "application/vnd.docker.distribution.manifest.list.v2+json";
inline constexpr const char* kMediaDockerManifest = "application/vnd.docker.distribution.manifest.v2+json";

// ── Bearer auth challenge (RFC 6750 / Docker token flow) ───────────────────
struct BearerChallenge {
    bool        ok = false;     ///< a Bearer challenge was parsed
    std::string realm;          ///< token endpoint URL
    std::string service;        ///< service= param
    std::string scope;          ///< scope= param (may be empty)
};

/// Parse a `WWW-Authenticate:` header value. Handles the full response-header
/// blob (finds the Bearer line) or a bare challenge. Returns ok=false when no
/// `Bearer` scheme is present (e.g. a plain 401 or Basic-only).
BearerChallenge parse_bearer_challenge(const std::string& www_authenticate);

/// Build the token request URL: `<realm>?service=<service>&scope=<scope>`
/// (params URL-encoded; scope omitted when empty).
std::string build_token_url(const BearerChallenge& c);

/// Extract the bearer token from a token-endpoint JSON response. Registries
/// return it as either `token` or `access_token`. Returns "" on parse failure.
std::string parse_token_response(const std::string& json);

// ── Image descriptors ──────────────────────────────────────────────────────
struct Descriptor {
    std::string media_type;
    std::string digest;         ///< "sha256:<64 hex>"
    long long   size = 0;       ///< bytes
};

struct PlatformDescriptor : Descriptor {
    std::string os;             ///< e.g. "linux"
    std::string arch;           ///< e.g. "arm64"
    std::string variant;        ///< e.g. "v8" (often empty)
};

struct ImageManifest {
    bool                    ok = false;
    Descriptor              config;     ///< image config blob
    std::vector<Descriptor> layers;     ///< ordered base → top
};

/// True when the manifest JSON is a manifest-list / image-index (has a
/// top-level `manifests` array) rather than a single image manifest.
bool manifest_is_index(const std::string& json);

/// Parse a manifest-list / image-index into its per-platform entries. Returns
/// an empty vector on parse failure.
std::vector<PlatformDescriptor> parse_manifest_index(const std::string& json);

/// Choose the entry matching (os, arch, variant). Exact os+arch is required; a
/// requested variant prefers an exact variant match but falls back to an
/// entry with no/empty variant. Returns -1 when nothing matches.
int select_platform(const std::vector<PlatformDescriptor>& entries,
                    const std::string& os, const std::string& arch,
                    const std::string& variant);

/// Parse a single image manifest (config + ordered layers). ok=false on
/// failure.
ImageManifest parse_image_manifest(const std::string& json);

// ── Digest + storage helpers ───────────────────────────────────────────────
/// Strictly validate a content digest: exactly "sha256:" followed by 64
/// lowercase hex chars, no surrounding whitespace. (Guards the OTA `\n`-in-sha
/// class of bug — a digest read from anywhere is rejected unless it is clean.)
bool is_valid_digest(const std::string& digest);

/// Local content-addressed path for a blob: `<root>/blobs/sha256/<hex>`.
/// Returns "" when `digest` is not a valid sha256 digest.
std::string blob_path(const std::string& root, const std::string& digest);

// ── Host platform (what to pull for this device) ───────────────────────────
inline const char* default_os() { return "linux"; }

inline const char* default_arch() {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "arm";
#elif defined(__x86_64__)
    return "amd64";
#else
    return "";
#endif
}

inline const char* default_variant() {
#if defined(__arm__) && !defined(__aarch64__)
    return "v7";
#else
    return "";
#endif
}

} // namespace containers

#endif /* __iot_container_registry_hpp__ */
