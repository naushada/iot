#ifndef __iot_container_registry_puller_hpp__
#define __iot_container_registry_puller_hpp__

#include <atomic>
#include <functional>
#include <string>

#include "image_ref.hpp"

/// Registry-v2 pull orchestration: stitches the pure containers_core protocol
/// helpers to the curl/OpenSSL http_client. Blocking — the daemon runs it on a
/// worker thread. See apps/docs/tdd-device-containers.md.

namespace containers {

struct PullResult {
    bool        ok = false;
    std::string image_id;          ///< image config digest ("sha256:…")
    std::string manifest_digest;   ///< platform image-manifest digest (for mount)
    long long   total_size = 0;    ///< config + layers, bytes
    std::string error;             ///< human message on failure
};

/// Progress callback: percent 0..100 + a short human detail string. Invoked on
/// the worker thread; the implementation must be thread-safe (the daemon writes
/// it straight to ds, which is).
using PullProgress = std::function<void(int pct, const std::string& detail)>;

/// Pull `ref` into the content-addressed store under `root`
/// (`<root>/blobs/sha256/<hex>` + `<root>/manifests/<image-id>.json`).
/// Handles bearer auth, manifest-index arch selection for this device, and
/// sha256-verifies every blob (cached blobs are reused). `user`/`pass` are
/// optional registry credentials. `cancel` is polled between blobs.
PullResult pull_image(const ImageRef&     ref,
                      const std::string&  user,
                      const std::string&  pass,
                      const std::string&  root,
                      const PullProgress& progress,
                      std::atomic<bool>&  cancel);

} // namespace containers

#endif /* __iot_container_registry_puller_hpp__ */
