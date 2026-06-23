-- container.* schema for the iot-containerd single-container runtime shim.
-- See apps/docs/tdd-device-containers.md.
--
-- Control plane (device-ui -> daemon). The UI writes these via /api/v1/db/set.
-- The *.request keys are monotonic tokens: bump (write a new value) to trigger
-- the action. iot-containerd records the value present at startup as a baseline
-- so a stale value does NOT fire a command on boot.
--   container.image.ref       - OCI/Docker ref to pull, e.g. docker.io/library/nginx:latest
--   container.registry.user   - optional registry username (write-only by convention)
--   container.registry.pass   - optional registry password/token (write-only by convention)
--   container.entrypoint      - override Entrypoint (JSON array or string; "" = image default)
--   container.cmd             - override CMD (JSON array or string; "" = image default)
--   container.limit.mem       - memory cap, e.g. "256M" ("" = unbounded)
--   container.limit.cpus      - CPU quota, e.g. "0.5" ("" = unbounded)
--   container.pull.request    - bump to pull container.image.ref
--   container.run.request     - bump to create + start the container
--   container.stop.request    - bump to stop + delete the container
--
-- Status plane (daemon -> device-ui). Written VOLATILE (live, no SD-card fsync;
-- resets on reboot — v1 has no autostart / persistence).
--   container.state           - idle|pulling|pulled|mounting|created|running|stopped|error
--   container.pull.progress   - 0..100 (percent of total layer bytes)
--   container.pull.detail     - current layer digest / status message
--   container.image.id        - resolved image config digest
--   container.image.size      - total image size in bytes (decimal string)
--   container.run.pid         - running container PID (decimal string)
--   container.run.started     - start timestamp
--   container.exit.code       - last container exit code (decimal string)
--   container.status          - human-readable status summary
--   container.error           - last error message ("" = none)
return {
    ["container.image.ref"]     = { access = "Admin",  type = "string", default = "" },
    ["container.registry.user"] = { access = "Admin",  type = "string", default = "" },
    ["container.registry.pass"] = { access = "Admin",  type = "string", default = "" },
    ["container.entrypoint"]    = { access = "Admin",  type = "string", default = "" },
    ["container.cmd"]           = { access = "Admin",  type = "string", default = "" },
    ["container.limit.mem"]     = { access = "Admin",  type = "string", default = "" },
    ["container.limit.cpus"]    = { access = "Admin",  type = "string", default = "" },
    ["container.pull.request"]  = { access = "Admin",  type = "string", default = "" },
    ["container.run.request"]   = { access = "Admin",  type = "string", default = "" },
    ["container.stop.request"]  = { access = "Admin",  type = "string", default = "" },

    ["container.state"]         = { access = "Viewer", type = "string", default = "idle" },
    ["container.pull.progress"] = { access = "Viewer", type = "string", default = "0" },
    ["container.pull.detail"]   = { access = "Viewer", type = "string", default = "" },
    ["container.image.id"]      = { access = "Viewer", type = "string", default = "" },
    ["container.image.size"]    = { access = "Viewer", type = "string", default = "" },
    ["container.run.pid"]       = { access = "Viewer", type = "string", default = "" },
    ["container.run.started"]   = { access = "Viewer", type = "string", default = "" },
    ["container.exit.code"]     = { access = "Viewer", type = "string", default = "" },
    ["container.status"]        = { access = "Viewer", type = "string", default = "" },
    ["container.error"]         = { access = "Viewer", type = "string", default = "" },
}
