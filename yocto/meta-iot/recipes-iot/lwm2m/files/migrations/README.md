# Config / schema migrations (`/usr/share/iot/migrations/`)

Ordered, idempotent shell scripts run by `iot-swupdate` after an OTA `opkg`
install, gated by the ds key `iot.config.version`. See
`apps/docs/tdd-yocto-swupdate.md` §11.

## When you need a migration

- **Adding a key** → *no migration*. A new schema key with a `default` is picked
  up automatically (the persisted store has no value → the daemon reads the
  schema default).
- **Deleting a key** → optional migration (prune the orphaned persisted value;
  otherwise it lingers harmlessly).
- **Renaming or retyping a key, or changing its meaning** → *migration required*.
  The store does not coerce or carry values across a rename/retype.

## Format

- File name: `NNNN-<slug>.sh`, `NNNN` zero-padded (e.g. `0001-rename-foo.sh`).
  `NNNN` is the config generation the migration produces.
- `iot-swupdate` runs every migration whose `NNNN` is greater than the current
  `iot.config.version`, in ascending order, then sets `iot.config.version=NNNN`.
- Must be **idempotent** (write the new value only if the old exists, etc.).
- Use `ds-cli --socket=/run/iot/data_store.sock` for all ds access. ds-server is
  restarted (new schema loaded) before migrations run, so `set` validates
  against the updated schema.
- Forward-only: a downgrade does not auto-reverse migrations.

See `0000-template.sh.example` for a starting point (it is `.example`, so it does
not match the `NNNN-*.sh` glob and never runs).
