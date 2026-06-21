-- mqtt.* schema for the iot-mqttd mirror daemon — publishes vehicle.* (and
-- future telemetry) to an operator-owned MQTT broker. See
-- apps/docs/tdd-vehicle-telemetry.md (Phase 3). This is a SEPARATE,
-- VPN-independent plane (device → broker over the WAN), not the cloud pipeline.
--
-- Read keys (operator → daemon). The daemon parks (no broker connection) while
-- mqtt.broker.host is empty, and connects as soon as it is set + saved, so it is
-- effectively disabled by default until configured. mqtt.mirror.enable gates
-- whether telemetry is actually published once connected.
--   mqtt.broker.host    - broker hostname/IP (empty → daemon parks)
--   mqtt.broker.port    - broker port (default 1883; 8883 for TLS)
--   mqtt.broker.user    - username (optional)
--   mqtt.broker.pass    - password (optional, write-only)
--   mqtt.client.id      - MQTT client id (empty → derived from iot.serial)
--   mqtt.mirror.enable  - publish vehicle telemetry when true (default false)
--   mqtt.topic.suffix   - topic = "<iot.serial>/<suffix>" (default "telemetry")
--   mqtt.qos            - publish QoS 0/1/2 (default 0)
return {
    ["mqtt.broker.host"]   = { access = "Admin", type = "string",  default = "" },
    ["mqtt.broker.port"]   = { access = "Admin", type = "integer", default = 1883 },
    ["mqtt.broker.user"]   = { access = "Admin", type = "string",  default = "" },
    ["mqtt.broker.pass"]   = { access = "Admin", type = "string",  default = "",
                               write_acl = {"gid:engineer"}, read_acl = {"gid:engineer"} },
    ["mqtt.client.id"]     = { access = "Admin", type = "string",  default = "" },
    ["mqtt.mirror.enable"] = { access = "Admin", type = "boolean", default = false },
    ["mqtt.topic.suffix"]  = { access = "Admin", type = "string",  default = "telemetry" },
    ["mqtt.qos"]           = { access = "Admin", type = "integer", default = 0 },
}
