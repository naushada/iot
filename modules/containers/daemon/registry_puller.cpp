#include "registry_puller.hpp"

#include <cstdio>
#include <unistd.h>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_stdio.h>
#include <ace/OS_NS_unistd.h>

#include "http_client.hpp"
#include "registry.hpp"

namespace containers {

namespace {

const int kManifestTimeout = 60;     // seconds
const int kBlobTimeout     = 600;    // seconds — layers can be large on slow links

std::string read_file(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

bool write_file(const std::string& path, const std::string& body) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    return ok;
}

bool file_exists(const std::string& path) {
    return ACE_OS::access(path.c_str(), F_OK) == 0;
}

std::vector<std::string> manifest_accepts() {
    return {kMediaOciIndex, kMediaDockerList, kMediaOciManifest, kMediaDockerManifest};
}

} // namespace

PullResult pull_image(const ImageRef&     ref,
                      const std::string&  user,
                      const std::string&  pass,
                      const std::string&  root,
                      const PullProgress& progress,
                      std::atomic<bool>&  cancel) {
    PullResult res;

    const std::string base    = "https://" + ref.registry;
    const std::string repo    = ref.repository;
    const std::string workdir = root + "/tmp";
    if (!mkdir_p(workdir) || !mkdir_p(root + "/blobs/sha256") ||
        !mkdir_p(root + "/manifests")) {
        res.error = "cannot create store under " + root;
        return res;
    }
    const std::string token_path = workdir + "/token.json";

    // Bearer token, cached + reused across manifest/blob GETs (same pull scope).
    std::string token;

    // GET with lazy bearer auth: on 401, parse the challenge, fetch a token
    // (HTTP Basic with creds when given), then retry once. Returns the final
    // HTTP status (0 on transport failure, with `err` set).
    auto authorized_get = [&](const std::string& url,
                              const std::vector<std::string>& accept,
                              const std::string& body_path,
                              bool follow, int timeout,
                              std::string& err) -> long {
        HttpResponse r;
        if (!http_get(url, accept, token, "", body_path, follow, timeout, r, err))
            return 0;
        if (r.status != 401) return r.status;

        auto ch = parse_bearer_challenge(r.headers);
        if (!ch.ok) { err = "401 without a Bearer challenge"; return r.status; }
        const std::string turl = build_token_url(ch);
        const std::string basic = user.empty() ? std::string() : (user + ":" + pass);
        HttpResponse tr;
        if (!http_get(turl, {}, "", basic, token_path, false, 30, tr, err)) {
            err = "token fetch failed: " + err;
            return 0;
        }
        if (tr.status / 100 != 2) {
            err = "token endpoint returned " + std::to_string(tr.status) +
                  (user.empty() ? " (try setting registry credentials)" : "");
            return tr.status;
        }
        token = parse_token_response(read_file(token_path));
        if (token.empty()) { err = "empty token from " + ch.realm; return tr.status; }

        HttpResponse r2;
        if (!http_get(url, accept, token, "", body_path, follow, timeout, r2, err))
            return 0;
        return r2.status;
    };

    // ── 1. Top manifest (tag or digest) ────────────────────────────────────
    const std::string ref_str = ref.digest.empty() ? ref.tag : ref.digest;
    const std::string manifest_path = workdir + "/manifest.json";
    std::string err;
    long st = authorized_get(base + "/v2/" + repo + "/manifests/" + ref_str,
                             manifest_accepts(), manifest_path, false,
                             kManifestTimeout, err);
    if (st == 0)        { res.error = "registry unreachable: " + err; return res; }
    if (st == 401 || st == 403) { res.error = "registry auth failed (" + std::to_string(st) +
                                              "): " + err; return res; }
    if (st == 404)      { res.error = "image/tag not found: " + repo + ":" + ref_str; return res; }
    if (st / 100 != 2)  { res.error = "manifest GET returned " + std::to_string(st); return res; }

    std::string body = read_file(manifest_path);
    res.manifest_digest = ref.digest;   // refined below for an index

    // ── 2. Resolve a manifest-list/index to this device's platform ─────────
    if (manifest_is_index(body)) {
        auto entries = parse_manifest_index(body);
        int idx = select_platform(entries, default_os(), default_arch(), default_variant());
        if (idx < 0) {
            res.error = std::string("no image for platform ") + default_os() + "/" +
                        default_arch();
            return res;
        }
        const std::string md = entries[idx].digest;
        if (!is_valid_digest(md)) { res.error = "index has a malformed digest"; return res; }
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] index → %C/%C manifest %C\n"),
                   default_os(), default_arch(), md.c_str()));
        st = authorized_get(base + "/v2/" + repo + "/manifests/" + md,
                            manifest_accepts(), manifest_path, false,
                            kManifestTimeout, err);
        if (st / 100 != 2) {
            res.error = "platform manifest GET returned " + std::to_string(st) + " " + err;
            return res;
        }
        body = read_file(manifest_path);
        res.manifest_digest = md;
    }

    // ── 3. Parse + validate the image manifest ─────────────────────────────
    ImageManifest im = parse_image_manifest(body);
    if (!im.ok) { res.error = "could not parse image manifest"; return res; }
    if (!is_valid_digest(im.config.digest)) {
        res.error = "image config has a malformed digest";
        return res;
    }
    for (const auto& l : im.layers) {
        if (!is_valid_digest(l.digest)) { res.error = "layer has a malformed digest"; return res; }
    }
    res.image_id = im.config.digest;

    // ── 4. Download + verify blobs (config first, then layers) ─────────────
    std::vector<Descriptor> blobs;
    blobs.push_back(im.config);
    for (const auto& l : im.layers) blobs.push_back(l);

    long long total = 0;
    for (const auto& b : blobs) total += (b.size > 0 ? b.size : 0);
    res.total_size = total;

    long long done = 0;
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        if (cancel.load()) { res.error = "cancelled"; return res; }

        const Descriptor& b = blobs[i];
        const std::string dest = blob_path(root, b.digest);
        const std::string hex  = b.digest.substr(7);   // after "sha256:"
        const std::string what = (i == 0 ? "config" : "layer " + std::to_string(i) +
                                                       "/" + std::to_string(blobs.size() - 1));
        auto pct = [&]() -> int {
            if (total > 0) return static_cast<int>(done * 100 / total);
            return static_cast<int>(i * 100 / blobs.size());
        };

        // Cache hit: present + digest still verifies.
        if (file_exists(dest)) {
            std::string have;
            if (sha256_file(dest, have) && have == hex) {
                done += (b.size > 0 ? b.size : 0);
                progress(pct(), "cached " + what);
                continue;
            }
        }

        progress(pct(), "pulling " + what);
        const std::string part = dest + ".part";
        st = authorized_get(base + "/v2/" + repo + "/blobs/" + b.digest,
                            {}, part, /*follow=*/true, kBlobTimeout, err);
        if (st / 100 != 2) {
            ::unlink(part.c_str());
            res.error = "blob " + b.digest + " GET returned " + std::to_string(st) +
                        (err.empty() ? "" : " " + err);
            return res;
        }

        std::string have;
        if (!sha256_file(part, have)) {
            ::unlink(part.c_str());
            res.error = "could not hash downloaded blob " + b.digest;
            return res;
        }
        if (have != hex) {
            ::unlink(part.c_str());
            res.error = "blob digest mismatch: expected sha256:" + hex +
                        " got sha256:" + have;
            return res;
        }
        if (ACE_OS::rename(part.c_str(), dest.c_str()) != 0) {
            ::unlink(part.c_str());
            res.error = "could not store blob " + b.digest;
            return res;
        }
        done += (b.size > 0 ? b.size : 0);
        progress(pct(), "verified " + what);
    }

    // ── 5. Persist the platform image manifest for the mount phase ─────────
    if (!write_file(root + "/manifests/" + im.config.digest.substr(7) + ".json", body)) {
        res.error = "could not persist manifest sidecar";
        return res;
    }

    progress(100, "pull complete");
    res.ok = true;
    return res;
}

} // namespace containers
