#ifndef __iot_container_oci_bundle_hpp__
#define __iot_container_oci_bundle_hpp__

#include <string>
#include <vector>

/// Pure OCI bundle generation — image-config parsing, CMD/Entrypoint
/// resolution, resource-limit parsing, and the runtime `config.json` (the OCI
/// runtime spec crun consumes). JSON via header-only nlohmann; no filesystem,
/// no crun, no ACE — host-unit-testable. The crun process lifecycle lives in
/// the daemon's crun_runtime. See apps/docs/tdd-device-containers.md.

namespace containers {

/// The parts of an OCI image config (the config blob's `config` object) the
/// runtime needs.
struct ImageConfig {
    bool                     ok = false;
    std::vector<std::string> env;          ///< "KEY=value" entries
    std::vector<std::string> entrypoint;
    std::vector<std::string> cmd;
    std::string              working_dir;  ///< "" → "/"
    std::string              user;         ///< "uid[:gid]" or a name
};

/// Parse an image config blob (JSON). Reads the nested `config` object's Env /
/// Entrypoint / Cmd / WorkingDir / User. ok=false on parse failure.
ImageConfig parse_image_config(const std::string& json);

/// Parse a UI override field into an argv vector. A value starting with '['
/// is parsed as a JSON string array; otherwise it is split on whitespace.
/// Empty → empty vector.
std::vector<std::string> parse_args_field(const std::string& field);

/// Docker arg resolution: effective entrypoint + effective cmd, where each is
/// the override when provided (its *_set flag true) else the image value.
std::vector<std::string> resolve_process_args(
    const std::vector<std::string>& img_entrypoint,
    const std::vector<std::string>& img_cmd,
    const std::vector<std::string>& ov_entrypoint, bool ov_entrypoint_set,
    const std::vector<std::string>& ov_cmd, bool ov_cmd_set);

/// Parse a memory limit like "256M" / "1g" / "536870912" into bytes. Suffixes
/// k/m/g (and ki/mi/gi) are honored, case-insensitively. Returns 0 for an
/// empty/invalid value (→ unbounded).
long long parse_mem_limit(const std::string& s);

/// Parse a CPU quota like "0.5" / "2" into a cgroup quota for `period`
/// microseconds (e.g. 0.5 @ 100000 → 50000). Returns 0 for empty/invalid
/// (→ unbounded).
long long parse_cpu_quota(const std::string& cpus, long long period);

/// Resolve an image `User` ("1000", "1000:1000", "root", "") into numeric
/// uid/gid. Non-numeric names fall back to 0:0 (named-user /etc/passwd lookup
/// is out of scope for v1).
void resolve_user(const std::string& user, unsigned& uid, unsigned& gid);

/// Inputs for the runtime config.json.
struct OciSpec {
    std::vector<std::string> args;            ///< process.args (non-empty)
    std::vector<std::string> env;
    std::string              cwd = "/";
    unsigned                 uid = 0;
    unsigned                 gid = 0;
    long long                mem_limit_bytes = 0;   ///< 0 → unbounded
    long long                cpu_quota = 0;         ///< 0 → unbounded
    long long                cpu_period = 100000;
    bool                     bind_resolv_conf = true; ///< host /etc/resolv.conf (ro)
    std::string              hostname = "iot-container";
};

/// Generate the OCI runtime config.json string. Host networking (no network
/// namespace), a Docker-like default capability set, NoNewPrivileges, the
/// standard mounts, and the cgroup resource limits from `spec`. `root.path` is
/// "rootfs" (relative to the bundle dir).
std::string generate_oci_config(const OciSpec& spec);

} // namespace containers

#endif /* __iot_container_oci_bundle_hpp__ */
