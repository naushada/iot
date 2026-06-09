#include "lwm2m_object_cert.hpp"

#include <cerrno>
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

/// Map an artifact type to the default file name under certDir.
const char* default_filename(const std::string& type) {
    if (type == "ca")   return "ca.crt";
    if (type == "cert") return "client.crt";
    if (type == "key")  return "client.key";
    return nullptr;
}

/// Default store_artifact: atomically write <certDir>/<file> (temp + rename),
/// 0600 for the private key, 0644 otherwise.
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
    const mode_t mode = (type == "key") ? 0600 : 0644;

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

/// Build one credential instance. RID 0 reports the fixed type; RID 1 stages
/// the PEM; RID 2 stages the hash. The Apply trigger (RID 3) lives only on
/// the instance flagged `withApply`.
ObjectInstance make_instance(std::uint32_t iid,
                             const std::string& type,
                             std::shared_ptr<Staging> staging,
                             bool withApply,
                             const std::string& certDir,
                             std::shared_ptr<CertHooks> hooks) {
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
    // RID 1 — Certificate Data (opaque, write-only; staged).
    {
        Resource r;
        r.rid = 1; r.name = "Certificate Data";
        r.type = ResourceType::Opaque; r.ops = Operations::W;
        r.write = [staging, type](const std::string& v) {
            auto& s = (*staging)[type];
            s.pem = v; s.present = true;
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [cert-obj] %M %N:%l staged %C data (%d bytes)\n"),
                type.c_str(), static_cast<int>(v.size())));
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
        r.execute = [staging, certDir, hooks](const std::string& /*args*/) -> int {
            int committed = 0;
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
                ++committed;
            }
            if (committed == 0) {
                ACE_ERROR_RETURN((LM_ERROR,
                    ACE_TEXT("%D [cert-obj] %M %N:%l apply with nothing staged\n")), -1);
            }
            // Clear staging so a later partial push can't re-commit stale bytes.
            staging->clear();
            int rc = hooks->apply ? hooks->apply() : 0;
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [cert-obj] %M %N:%l applied %d artifact(s), reload rc=%d\n"),
                committed, rc));
            return rc;
        };
        inst.resources[3] = std::move(r);
    }
    return inst;
}

} // namespace

int install_cert(ObjectStore& store,
                 const std::string& certDir,
                 CertHooks hooks) {
    auto staging = std::make_shared<Staging>();
    auto hooksp  = std::make_shared<CertHooks>(std::move(hooks));

    ObjectDescriptor desc;
    desc.oid              = kCertObjectOid;
    desc.name             = "Credential Provisioning";
    desc.urn              = "urn:iot:lwm2m:cred:2048:1.0";
    desc.multipleInstance = true;
    desc.mandatory        = false;

    // Apply trigger lives on instance 0 (the CA instance) so the server has
    // a single, well-known commit point: EXECUTE /2048/0/3.
    desc.instances[0] = make_instance(0, "ca",   staging, /*withApply*/true,  certDir, hooksp);
    desc.instances[1] = make_instance(1, "cert", staging, /*withApply*/false, certDir, hooksp);
    desc.instances[2] = make_instance(2, "key",  staging, /*withApply*/false, certDir, hooksp);

    store.add_object(std::move(desc));
    return 0;
}

}} // namespace lwm2m::objects
