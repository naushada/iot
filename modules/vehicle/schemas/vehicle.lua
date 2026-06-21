-- vehicle.* schema for the iot-vehicled CAN/OBD-II (ISO 15765-4) daemon.
-- See apps/docs/tdd-vehicle-telemetry.md.
--
-- Read keys (operator -> daemon):
--   vehicle.can.iface        - SocketCAN interface (default "can0")
--   vehicle.can.bitrate      - CAN bitrate (default 500000; 250000 variant)
--   vehicle.poll.interval.ms - PID poll cadence in ms (default 1000)
--
-- Write keys (daemon -> device-ui / lwm2m client). All string-typed; numeric
-- values are decimal strings (matches the iot.sensor.* / cell.* convention).
-- The live telemetry keys are written VOLATILE (in-memory, no fsync) since they
-- are transient per-tick readings; vehicle.dtc is persisted (on-change, low rate).
--   vehicle.rpm       - engine RPM
--   vehicle.speed     - vehicle speed (km/h)
--   vehicle.coolant   - coolant temperature (deg C)
--   vehicle.throttle  - throttle position (%)
--   vehicle.load      - calculated engine load (%)
--   vehicle.fuel      - fuel tank level (%)
--   vehicle.iat       - intake air temperature (deg C)
--   vehicle.maf       - mass air flow (g/s)
--   vehicle.dtc       - JSON array of active DTCs (Mode 03)
--   vehicle.link      - bus health: "up" / "down" / "no-ecu"
return {
    ["vehicle.can.iface"]        = { access = "Admin",  type = "string",  default = "can0" },
    ["vehicle.can.bitrate"]      = { access = "Admin",  type = "integer", default = 500000 },
    ["vehicle.poll.interval.ms"] = { access = "Admin",  type = "integer", default = 1000 },

    ["vehicle.rpm"]      = { access = "Viewer", type = "string", default = "" },
    ["vehicle.speed"]    = { access = "Viewer", type = "string", default = "" },
    ["vehicle.coolant"]  = { access = "Viewer", type = "string", default = "" },
    ["vehicle.throttle"] = { access = "Viewer", type = "string", default = "" },
    ["vehicle.load"]     = { access = "Viewer", type = "string", default = "" },
    ["vehicle.fuel"]     = { access = "Viewer", type = "string", default = "" },
    ["vehicle.iat"]      = { access = "Viewer", type = "string", default = "" },
    ["vehicle.maf"]      = { access = "Viewer", type = "string", default = "" },
    ["vehicle.dtc"]      = { access = "Viewer", type = "string", default = "" },
    ["vehicle.link"]     = { access = "Viewer", type = "string", default = "down" },
}
