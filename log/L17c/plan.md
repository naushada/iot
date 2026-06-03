# L17c Plan — per-key ACL

> **Status (2026-06-03):** **CLOSED.** All shipped in one commit.

Restricts which peers can write specific keys using Unix peer
credentials (`SO_PEERCRED` / `getpeereid`). Schema keys gain
optional `write_acl` and `read_acl` arrays.

## Design

1. **Session**: captures peer uid/gid via `getpeereid()` at accept
   time. Stored as `m_uid`/`m_gid`; exposed via `peer_uid()`/`peer_gid()`.
2. **SchemaEntry**: gains `write_acl` and `read_acl` vectors of
   strings (`"uid:N"` or `"gid:name"`). Parsed from Lua schema.
3. **SchemaRegistry::check_write_acl()**: checks peer credentials
   against the key's ACL. Empty ACL = unrestricted. Unknown key =
   passthrough.
4. **Worker::Set handler**: calls `check_write_acl()` after schema
   validation, before touching the store.
5. **Default policy** (`services.lua`): all `services.*.enable` keys
   are `write_acl = {"uid:0"}` (root-only).

## Usage

```sh
# As root: accepted
sudo ds-cli svc disable openvpn.client

# As non-root: rejected
ds-cli svc disable openvpn.client
# → SchemaRejected: write_acl(...): access denied for uid=1000
```

To delegate to a group:
```lua
write_acl = {"uid:0", "gid:iot-operators"}
```

## Non-goals (v1)
- Read ACL enforcement (reads remain unrestricted)
- Per-operation ACL granularity (same ACL for regular + volatile set)
- Token-based auth (relies on kernel peer credentials)
- Dynamic ACL reload (ACL changes require ds-server restart)
