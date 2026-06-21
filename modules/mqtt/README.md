# modules/mqtt — `iot-mqttd` MQTT telemetry mirror

Mirrors device telemetry (`vehicle.*`, and future signals) to an
operator-owned MQTT broker. A **separate, VPN-independent plane** — the device
publishes to the broker over the WAN, *not* through the LwM2M control plane or
the OpenVPN tunnel. See `apps/docs/tdd-vehicle-telemetry.md` (Phase 3).

## Layout

| Path | Role |
| --- | --- |
| `daemon/mqtt_mirror.{hpp,cpp}` | `iot-mqttd` — ACE-reactor daemon; an ACE timer drives libmosquitto's `mosquitto_loop()` (network + keepalive + reconnect in one call), a second timer publishes telemetry. |
| `daemon/main.cpp` | argv (`--ds-sock`, `--interval`) entry point. |
| `schemas/mqtt.lua` | `mqtt.*` config keys (broker, mirror toggle, topic, QoS). |

## Behaviour

- **Off by default:** parks (no broker connection) until `mqtt.broker.host` is
  set, retrying every ~5 s while disconnected. The device-ui MQTT page writes
  the config; the daemon connects on the next tick.
- **Topic:** `<iot.serial>/<mqtt.topic.suffix>` (default suffix `telemetry`).
- **Payload:** one JSON object per poll of the `vehicle.*` keys, published with
  `retain=true` (a late subscriber gets the last frame). `vehicle.dtc` is
  embedded as a raw JSON array; other fields are quoted strings.
- **Mirror gate:** `mqtt.mirror.enable` (re-read live) gates publishing, so the
  broker connection can exist without mirroring.

## Build

`MQTT_BUILD_DAEMON` is **OFF by default** — only the device Yocto build enables
it (`-DMQTT_BUILD_DAEMON=ON` + `DEPENDS mosquitto` in `iot_git.bb`), because the
cloud Docker builder has no libmosquitto. A plain `add_subdirectory` from
`apps/CMakeLists.txt` just installs the `mqtt.lua` schema.

```sh
# device image: recipe passes -DMQTT_BUILD_DAEMON=ON; links -lmosquitto.
# packaged as ${PN}-mqtt (binary + iot-mqttd.service, enabled but parked).
```

## ds keys

```
mqtt.broker.host / .port / .user / .pass   broker endpoint + creds (pass write-only)
mqtt.client.id                              MQTT client id (empty → iot.serial)
mqtt.mirror.enable                          publish telemetry when true (default false)
mqtt.topic.suffix                           topic = <serial>/<suffix> (default "telemetry")
mqtt.qos                                     publish QoS 0/1/2 (default 0)
```
