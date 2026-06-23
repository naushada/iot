#ifndef __iot_container_crun_runtime_hpp__
#define __iot_container_crun_runtime_hpp__

#include <string>
#include <vector>

/// Thin wrapper around the `crun` OCI runtime CLI (via ACE_Process), mirroring
/// the http_client/curl pattern. The daemon drives a single container through
/// create → start → (supervise via state) → kill → delete. Container state
/// lives under a dedicated `--root` dir. See apps/docs/tdd-device-containers.md.

namespace containers {

struct CrunStatus {
    bool        exists = false;   ///< `crun state` found the container
    std::string status;          ///< "creating"|"created"|"running"|"stopped"
    long        pid = 0;         ///< container PID (host view), 0 when none
};

class CrunRuntime {
public:
    /// `state_root` is passed as `crun --root <dir>` — where crun keeps its
    /// per-container state (must be on a writable, preferably tmpfs, path).
    explicit CrunRuntime(std::string state_root);

    /// `crun create --bundle <bundle_dir> <id>` — set up namespaces/cgroups
    /// from <bundle_dir>/config.json, paused before the user process.
    bool create(const std::string& id, const std::string& bundle_dir, std::string& err);

    /// `crun start <id>` — exec the container's process.
    bool start(const std::string& id, std::string& err);

    /// `crun state <id>` — current status + pid (exists=false when gone).
    CrunStatus state(const std::string& id);

    /// `crun kill <id> <signal>` — signal is "TERM" / "KILL" / etc.
    bool kill(const std::string& id, const std::string& signal, std::string& err);

    /// `crun delete [--force] <id>` — drop the container's state.
    bool remove(const std::string& id, bool force, std::string& err);

private:
    int run(const std::vector<std::string>& args, std::string* out, std::string& err);

    std::string m_state_root;
    std::string m_crun;   ///< resolved crun binary path
};

} // namespace containers

#endif /* __iot_container_crun_runtime_hpp__ */
