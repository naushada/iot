#include "lwm2m_object_cert.hpp"
#include "lwm2m_cert_chunk.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>

#include <ace/Log_Msg.h>

namespace lwm2m { namespace objects {

namespace {

/// One staged artifact: the PEM bytes plus an optional integrity hash,
/// held until the Apply EXECUTE commits the whole family at once.
struct Staged {
    std::string pem;
    std::string hash;     ///< lowercase hex SHA-256, empty if not written
    bool        present{false};
};

/// Shared staging area captured by every instance's WRITE closures and the
/// instance-0 Apply EXECUTE closure. Keyed by artifact type ("ca"/"cert"/
/// "key"). shared_ptr so all the std::function captures see the same map.
using Staging = std::map<std::string, Staged>;

/// Cloud-pushed VPN server endpoint (Object 2048 /0/5,6,7), staged like the
/// cert artifacts and materialised into the data store at Apply via
/// CertHooks::set_vpn_endpoint. Empty host → no endpoint was pushed.
struct VpnEndpoint {
    std::string host;
    std::string port;
    std::string proto;
};

/// Map an artifact type to the default file name under certDir.
const char* default_filename(const std::string& type) {
    if (type == "ca")   return "ca.crt";
    if (type == "cert") return "client.crt";
    if (type == "key")  return "client.key";
    return nullptr;
}

/// Default store_artifact: atomically write <certDir>/<file> (temp + rename),
/// 0640 for the private key, 0644 otherwise. The key is group-readable (not
/// 0600) so openvpn-client — a separate DynamicUser sharing group `iot` via the
/// 2750 engineer:iot certDir — can read it; the writer (lwm2m client, `engineer`)
/// owns it. World still has no access to the key.
int default_store(const std::string& certDir,
                  const std::string& type,
                  const std::string& pem) {
    const char* file = default_filename(type);
    if (!file) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cert-obj] %M %N:%l unknown artifact type '%C'\n"),
            type.c_str()), -1);
    }
    std::string dir = certDir;
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    const std::string path = dir + file;
    const std::string tmp  = path + ".tmp";
    const mode_t mode = (type == "key") ? 0640 : 0644;

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cert-obj] %M %N:%l open(%C) failed errno=%d\n"),
            tmp.c_str(), errno), -1);
    }
    ssize_t off = 0;
    const ssize_t n = static_cast<ssize_t>(pem.size());
    while (off < n) {
        ssize_t w = ::write(fd, pem.data() + off, static_cast<size_t>(n - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd); ::unlink(tmp.c_str());
            ACE_ERROR_RETURN((LM_ERROR,
                ACE_TEXT("%D [cert-obj] %M %N:%l write(%C) failed errno=%d\n"),
                tmp.c_str(), errno), -1);
        }
        off += w;
    }
    ::fchmod(fd, mode);
    if (::close(fd) < 0) { ::unlink(tmp.c_str()); return -1; }
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        ::unlink(tmp.c_str());
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cert-obj] %M %N:%l rename(%C) failed errno=%d\n"),
            path.c_str(), errno), -1);
    }
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [cert-obj] %M %N:%l wrote %C (%d bytes, type=%C)\n"),
        path.c_str(), static_cast<int>(pem.size()), type.c_str()));
    return 0;
}

/// Artifact types that make a complete VPN credential family, in the order
/// the fingerprint concatenates them.
const char* const kFamilyTypes[3] = {"ca", "cert", "key"};

/// Stable 64-bit FNV-1a over `s`, lowercase 16-hex. Deterministic across
/// processes (unlike std::hash) so a server can compute the same fingerprint
/// of the family it pushed and compare against RID 4.
std::string fnv1a_hex(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

/// Build one credential instance. RID 0 reports the fixed type; RID 1 stages
/// the PEM; RID 2 stages the hash. The Apply trigger (RID 3) + the Applied
/// fingerprint (RID 4) live only on the instance flagged `withApply`.
ObjectInstance make_instance(std::uint32_t iid,
                             const std::string& type,
                             std::shared_ptr<Staging> staging,
                             bool withApply,
                             const std::string& certDir,
                             std::shared_ptr<CertHooks> hooks,
                             bool require_complete,
                             std::shared_ptr<std::string> applied,
                             std::shared_ptr<VpnEndpoint> vpn) {
    ObjectInstance inst;
    inst.iid = iid;

    // RID 0 — Certificate Type (read-only identity of this instance).
    {
        Resource r;
        r.rid = 0; r.name = "Certificate Type";
        r.type = ResourceType::String; r.ops = Operations::R;
        r.read = [type]() { return type; };
        inst.resources[0] = std::move(r);
    }
    // RID 1 — Certificate Data (opaque, write-only). The server zips + chunks
    // each artifact (large PEMs exceed a DTLS record), so each WRITE is one
    // chunk; reassemble + inflate per instance, then stage the full PEM.
    {
        auto reasm = std::make_shared<::lwm2m::certchunk::Reassembler>();
        Resource r;
        r.rid = 1; r.name = "Certificate Data";
        r.type = ResourceType::Opaque; r.ops = Operations::W;
        r.write = [staging, type, reasm](const std::string& v) -> int {
            std::string pem;
            int rc = reasm->feed(v, pem);
            if (rc < 0) {
                ACE_ERROR_RETURN((LM_ERROR,
                    ACE_TEXT("%D [cert-obj] %M %N:%l malformed %C chunk\n"),
                    type.c_str()), -1);
            }
            if (rc == 0) {
                ACE_DEBUG((LM_DEBUG,
                    ACE_TEXT("%D [cert-obj] %M %N:%l buffered %C chunk (waiting "
                             "for more)\n"), type.c_str()));
                return 0;   // more chunks to come
            }
            auto& s = (*staging)[type];
            s.pem = std::move(pem); s.present = true;
            ACE_DEBUG((LM_DEBUG,
                ACE_TEXT("%D [cert-obj] %M %N:%l reassembled %C (%d bytes)\n"),
                type.c_str(), static_cast<int>(s.pem.size())));
            return 0;
        };
        inst.resources[1] = std::move(r);
    }
    // RID 2 — Hash (write-only; staged for optional verify at commit).
    {
        Resource r;
        r.rid = 2; r.name = "Hash";
        r.type = ResourceType::String; r.ops = Operations::W;
        r.write = [staging, type](const std::string& v) {
            (*staging)[type].hash = v;
            return 0;
        };
        inst.resources[2] = std::move(r);
    }
    // RID 3 — Apply (execute; commit the whole staged family + reload).
    if (withApply) {
        Resource r;
        r.rid = 3; r.name = "Apply";
        r.type = ResourceType::None; r.ops = Operations::E;
        r.execute = [staging, certDir, hooks, require_complete, applied, vpn]
                    (const std::string& /*args*/) -> int {
            // Completeness gate: don't half-provision. If we require a full
            // family, hold the commit until ca+cert+key are all staged. The
            // partial set stays staged so a later Apply (after the missing
            // WRITE is re-pushed) commits the whole family — making the
            // server's re-push loss-tolerant.
            if (require_complete) {
                for (auto* t : kFamilyTypes) {
                    auto it = staging->find(t);
                    if (it == staging->end() || !it->second.present) {
                        ACE_DEBUG((LM_DEBUG,
                            ACE_TEXT("%D [cert-obj] %M %N:%l apply deferred: '%C' "
                                     "not staged yet; retaining partial family\n"),
                            t));
                        return -1;   // not applied (server will re-push)
                    }
                }
            }
            // The cloud-pushed VPN endpoint (RID 5/6/7) is folded into the
            // idempotency fingerprint below. It is NOT a cert PEM, but it IS
            // part of "what's currently applied" — without it, a proto/host/port
            // change that keeps the same cert family would hash identically and
            // be silently skipped, so the device never learns the new endpoint
            // (e.g. an operator flipping cloud.vpn.proto tcp↔udp). The unit
            // separator keeps the three fields unambiguous against PEM bytes.
            const std::string ep_suffix =
                vpn ? ("\x1f" + vpn->host + "\x1f" + vpn->port + "\x1f" + vpn->proto)
                    : std::string();
            // Idempotency gate: the cloud re-pushes the cert family every ~30s
            // until the tunnel reports up, which re-triggers this Apply. If the
            // staged family + endpoint is byte-identical to what's already
            // applied (RID 4 fingerprint), skip the store + reload entirely —
            // otherwise we bounce openvpn every 30s and it can never finish a
            // handshake. The fp is computed over the staged PEMs in the same
            // ca,cert,key order as the committed one below, plus ep_suffix.
            if (applied && !applied->empty()) {
                std::string cand;
                bool any_pem = false;
                for (auto& [type, s] : *staging) {
                    if (s.present) { cand += s.pem; any_pem = true; }
                }
                if (any_pem) cand += ep_suffix;
                if (any_pem && fnv1a_hex(cand) == *applied) {
                    ACE_DEBUG((LM_INFO,
                        ACE_TEXT("%D [cert-obj] %M %N:%l apply skipped: family + "
                                 "endpoint unchanged (fp=%C) — not reloading\n"),
                        applied->c_str()));
                    staging->clear();
                    return 0;   // already applied; no store, no reload
                }
            }
            int committed = 0;
            std::string fpInput;     // FNV-1a input: PEMs in ca,cert,key order
            for (auto& [type, s] : *staging) {
                if (!s.present) continue;
                if (hooks->verify && !s.hash.empty()) {
                    if (hooks->verify(type, s.hash, s.pem) != 0) {
                        ACE_ERROR_RETURN((LM_ERROR,
                            ACE_TEXT("%D [cert-obj] %M %N:%l hash verify failed "
                                     "for %C; aborting apply\n"), type.c_str()), -1);
                    }
                }
                int rc = hooks->store_artifact
                       ? hooks->store_artifact(type, s.pem)
                       : default_store(certDir, type, s.pem);
                if (rc != 0) return -1;
                fpInput += s.pem;    // map is sorted → ca, cert, key
                ++committed;
            }
            if (committed == 0) {
                ACE_ERROR_RETURN((LM_ERROR,
                    ACE_TEXT("%D [cert-obj] %M %N:%l apply with nothing staged\n")), -1);
            }
            // Clear staging so a later partial push can't re-commit stale bytes.
            staging->clear();
            // Fold the VPN endpoint into the committed fingerprint, matching the
            // candidate hashed in the idempotency gate above — so a later
            // endpoint-only change (same certs) hashes differently and applies.
            fpInput += ep_suffix;
            if (applied) *applied = fnv1a_hex(fpInput);
            // Materialise the cloud-pushed VPN endpoint (if any) into ds before
            // the reload hook, so openvpn (re)starts against the right server.
            if (vpn && !vpn->host.empty() && hooks->set_vpn_endpoint) {
                int vrc = hooks->set_vpn_endpoint(vpn->host, vpn->port, vpn->proto);
                ACE_DEBUG((LM_INFO,
                    ACE_TEXT("%D [cert-obj] %M %N:%l applied VPN endpoint "
                             "host=%C port=%C proto=%C rc=%d\n"),
                    vpn->host.c_str(), vpn->port.c_str(),
                    vpn->proto.c_str(), vrc));
            }
            int rc = hooks->apply ? hooks->apply() : 0;
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [cert-obj] %M %N:%l applied %d artifact(s) fp=%C, "
                         "reload rc=%d\n"),
                committed, applied ? applied->c_str() : "", rc));
            return rc;
        };
        inst.resources[3] = std::move(r);

        // RID 4 — Applied: stable fingerprint of the last committed family
        // (empty until the first successful Apply). A server READs this to
        // confirm delivery + detect a re-mint.
        Resource st;
        st.rid = 4; st.name = "Applied";
        st.type = ResourceType::String; st.ops = Operations::R;
        st.read = [applied]() { return applied ? *applied : std::string(); };
        inst.resources[4] = std::move(st);

        // RID 5/6/7 — VPN server endpoint (write-only; staged, materialised at
        // Apply). The cloud writes these before the Apply EXECUTE so the device
        // learns its tunnel target without a docker-compose seed.
        {
            Resource r;
            r.rid = 5; r.name = "VPN Remote Host";
            r.type = ResourceType::String; r.ops = Operations::W;
            r.write = [vpn](const std::string& v) { if (vpn) vpn->host = v; return 0; };
            inst.resources[5] = std::move(r);
        }
        {
            Resource r;
            r.rid = 6; r.name = "VPN Remote Port";
            r.type = ResourceType::String; r.ops = Operations::W;
            r.write = [vpn](const std::string& v) { if (vpn) vpn->port = v; return 0; };
            inst.resources[6] = std::move(r);
        }
        {
            Resource r;
            r.rid = 7; r.name = "VPN Remote Proto";
            r.type = ResourceType::String; r.ops = Operations::W;
            r.write = [vpn](const std::string& v) { if (vpn) vpn->proto = v; return 0; };
            inst.resources[7] = std::move(r);
        }
    }
    return inst;
}

} // namespace

int install_cert(ObjectStore& store,
                 const std::string& certDir,
                 CertHooks hooks,
                 bool require_complete) {
    auto staging = std::make_shared<Staging>();
    auto hooksp  = std::make_shared<CertHooks>(std::move(hooks));
    auto applied = std::make_shared<std::string>();   // RID 4 fingerprint
    auto vpn     = std::make_shared<VpnEndpoint>();    // RID 5/6/7 staging

    ObjectDescriptor desc;
    desc.oid              = kCertObjectOid;
    desc.name             = "Credential Provisioning";
    desc.urn              = "urn:iot:lwm2m:cred:2048:1.0";
    desc.multipleInstance = true;
    desc.mandatory        = false;

    // Apply trigger + Applied status live on instance 0 (the CA instance) so
    // the server has a single, well-known commit point: EXECUTE /2048/0/3,
    // READ /2048/0/4.
    desc.instances[0] = make_instance(0, "ca",   staging, /*withApply*/true,  certDir, hooksp, require_complete, applied, vpn);
    desc.instances[1] = make_instance(1, "cert", staging, /*withApply*/false, certDir, hooksp, require_complete, applied, vpn);
    desc.instances[2] = make_instance(2, "key",  staging, /*withApply*/false, certDir, hooksp, require_complete, applied, vpn);

    store.add_object(std::move(desc));
    return 0;
}

}} // namespace lwm2m::objects
