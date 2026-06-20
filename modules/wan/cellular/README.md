# cellular — mangOH Yellow cellular WAN + GPS

Drives the mangOH Yellow's CF3 cellular module (Sierra Wireless WP / Quectel) as
the Pi's **primary WAN uplink** and its **GPS/GNSS** source, publishing status to
the data-store. Sibling of `modules/wan/wifi/client`. Part of the WAN plane in
[`apps/docs/tdd-mangoh-yellow-sensors.md`](../../../apps/docs/tdd-mangoh-yellow-sensors.md) §6.

## Layout

```
inc/at_parser.hpp    pure +CSQ/+COPS/+CREG/+CGPADDR/ICCID parsers
inc/nmea_parser.hpp  pure $--GGA / $--RMC NMEA parsers (+ XOR checksum)
inc/cell_state.hpp   CellularState cache → cell.* / gps.* to_kv() batch
src/                 implementations (no I/O — host-testable)
schemas/cell.lua     cell.* (modem status) + gps.* (location) ds keys
test/                gtests over the parsers + state (run in podman)
daemon/              cellular-client — the privileged producer (PR-C)
```

`cellular_core` (the lib) is **pure** — no ACE, no data-store — so the AT/NMEA
parsing and state are fully host-unit-tested. The serial/modem control and ds
publishing live in the `cellular-client` daemon (PR-C), which is **reactor-driven
and ACE-only**: the AT + GNSS ttys are `ACE_TTY_IO`/`ACE_DEV_IO` handles
registered with the `ACE_Reactor` (`handle_input` → parsers), with a reactor
timer for the periodic `+CSQ/+COPS/+CREG` polls — no raw POSIX I/O.

## Build & test

```bash
cmake -S modules/wan/cellular -B modules/wan/cellular/build -DCELLULAR_BUILD_TESTS=ON
cmake --build modules/wan/cellular/build
ctest --test-dir modules/wan/cellular/build
```

## Data flow

`cellular-client` → `cell.*` / `gps.*` in ds → device-ui (local) + the lwm2m
client mirrors `gps.*` into **LwM2M Object 6 (Location)** for the cloud (PR-B).
net-router already routes the cellular slot (`net.iface.cellular.name`/`wwan0`).
