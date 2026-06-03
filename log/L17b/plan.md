# L17b Plan — ephemeral disable

> **Status (2026-06-03):** **CLOSED.** All shipped in one commit.

Adds `ds-cli svc disable --until-boot` — a volatile set that lives
in an in-memory overlay in ds-server (not persisted to data_store.lua).
On reboot, the persistent default (`enable=true`) takes effect.

## Design

Four changes:
1. **DataStore** (`data_store.hpp/cpp`): `m_volatile` overlay map.
   `set_volatile()` writes to overlay (no persist). `get()` checks
   volatile first, then persistent. Normal `set()` clears volatile.
2. **Worker** (`worker.cpp`): Set handler checks `req["volatile"]`,
   calls `set_volatile()` instead of `set()` when true.
3. **Client API** (`client.hpp/cpp`): `set_volatile(k, v)` sends
   `{"volatile":true,"keys":[...]}`.
4. **ds-cli** (`ds_cli.cpp`): `svc disable --until-boot` invokes
   `set_volatile`. `svc enable` always does a persistent set
   (which clears the volatile entry).

## Usage

```sh
# Temporary: gone after ds-server restart
ds-cli svc disable --until-boot openvpn.client

# Permanent: persists to data_store.lua
ds-cli svc disable openvpn.client

# Revert either form
ds-cli svc enable openvpn.client
```

## Non-goals (v1)
- `--until` with a wall-clock time or duration ("disable for 5m")
- Volatile-only keys (every key can be both volatile and persistent)
- Volatile get/status distinction (get() returns the volatile value
  seamlessly; operator doesn't need to know if it's volatile or not)
