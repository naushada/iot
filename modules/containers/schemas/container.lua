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
--   container.net.mode        - "host" (default; shares device netns/IP) | "bridge" (own IP)
--   container.net.subnet      - bridge /24, default "10.88.0.0/24" (bridge mode only)
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
--   container.net.ip          - container IP in bridge mode ("" in host mode / stopped)
--   container.net.gateway     - bridge gateway (container default route)
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
    ["container.net.mode"]      = { access = "Admin",  type = "string", default = "host" },
    ["container.net.subnet"]    = { access = "Admin",  type = "string", default = "10.88.0.0/24" },
    ["container.pull.request"]  = { access = "Admin",  type = "string", default = "" },
    ["container.run.request"]   = { access = "Admin",  type = "string", default = "" },
    ["container.stop.request"]  = { access = "Admin",  type = "string", default = "" },

    -- ── Phase 2: multi-container (dynamic named) ──────────────────────────
    -- The ds schema is static (no per-name keys), so multiple containers are
    -- carried as ONE JSON document the daemon publishes and the UI grid renders,
    -- plus a single command envelope routed by name (mirrors the cloud's
    -- cloud.endpoint.credentials JSON-array pattern). The legacy singular keys
    -- above remain as a view of the most-recently-touched container during the
    -- daemon migration.
    --
    --   container.instances  - JSON array; one object per container. This is the
    --       durable roster the daemon reads back on startup to rehydrate its map
    --       (persistent set(), NOT volatile — so it survives an OTA/ds restart;
    --       the daemon reconciles each entry against live crun state on boot).
    --       Carries the config needed to fully reconstruct a container
    --       (image/net/subnet/mem/cpus/entrypoint/cmd) plus the live view, e.g.
    --       [{"name":"web","image":"docker.io/library/nginx:latest",
    --         "state":"running","ip":"10.88.0.2","gateway":"10.88.0.1",
    --         "net":"bridge","subnet":"10.88.0.0/24","mem":"256M","cpus":"0.5",
    --         "entrypoint":"","cmd":"","pid":1234,"exitCode":null,"error":""}]
    --   container.cmd.*      - command envelope; set the fields then bump
    --                          container.cmd.request to execute one action.
    ["container.instances"]     = { access = "Viewer", type = "string", default = "[]" },
    ["container.cmd.request"]   = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.name"]      = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.action"]    = { access = "Admin",  type = "string", default = "" },  -- pull|run|stop|remove|logs|prune (prune is store-wide, needs no name)
    ["container.cmd.image"]     = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.entrypoint"]= { access = "Admin",  type = "string", default = "" },
    ["container.cmd.cmd"]       = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.mem"]       = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.cpus"]      = { access = "Admin",  type = "string", default = "" },
    ["container.cmd.net"]       = { access = "Admin",  type = "string", default = "host" },
    ["container.cmd.subnet"]    = { access = "Admin",  type = "string", default = "10.88.0.0/24" },

    ["container.state"]         = { access = "Viewer", type = "string", default = "idle" },
    ["container.pull.progress"] = { access = "Viewer", type = "string", default = "0" },
    ["container.pull.detail"]   = { access = "Viewer", type = "string", default = "" },
    ["container.image.id"]      = { access = "Viewer", type = "string", default = "" },
    ["container.image.size"]    = { access = "Viewer", type = "string", default = "" },
    ["container.net.ip"]        = { access = "Viewer", type = "string", default = "" },
    ["container.net.gateway"]   = { access = "Viewer", type = "string", default = "" },
    ["container.run.pid"]       = { access = "Viewer", type = "string", default = "" },
    ["container.run.started"]   = { access = "Viewer", type = "string", default = "" },
    ["container.exit.code"]     = { access = "Viewer", type = "string", default = "" },
    ["container.status"]        = { access = "Viewer", type = "string", default = "" },
    ["container.error"]         = { access = "Viewer", type = "string", default = "" },

    -- Container console log (stdout+stderr), published on demand by the
    -- `logs` command envelope: the daemon writes the tail (last 16 KB) of the
    -- named container's log for the device-ui Logs viewer. Admin-gated — output
    -- may contain application secrets.
    ["container.log"]           = { access = "Admin",  type = "string", default = "" },
    ["container.log.name"]      = { access = "Admin",  type = "string", default = "" },
}
