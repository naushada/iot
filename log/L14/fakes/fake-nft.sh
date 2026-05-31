#!/bin/sh
# Fake nft(8) for the L14 end-to-end smoke.
#
# net-router invokes `nft -f <tempfile>` via apply::default_nft_apply.
# This script copies the tempfile to a host-visible recorder dir + exits
# 0 so the daemon thinks the apply succeeded. The smoke later asserts
# that the recorded ruleset contains the expected substrings (target IP,
# DNAT for ports 80/443/5684, etc.).
#
# Same shape as the FakeNft used in modules/net/router/test/apply_test.cpp,
# minus the per-test rc/stderr knobs — the smoke only exercises the
# happy path.

set -eu

RECORDER_DIR="${FAKE_RECORDER_DIR:-/recorder}"
mkdir -p "$RECORDER_DIR"

# argv pattern: `nft -f <path>`. Anything else (e.g. `nft list table`)
# is logged but otherwise ignored so a future smoke that lists tables
# doesn't trip.
if [ "${1:-}" = "-f" ] && [ -n "${2:-}" ]; then
    cp "$2" "$RECORDER_DIR/last_ruleset.nft"
    # Append-only history so a regression can scan every applied ruleset.
    cat "$2" >> "$RECORDER_DIR/ruleset_history.nft"
    printf '\n# --- apply %s ---\n' "$(date -u +%FT%TZ)" \
        >> "$RECORDER_DIR/ruleset_history.nft"
else
    printf 'fake-nft: ignored argv:' >> "$RECORDER_DIR/fake-nft.log"
    for a in "$@"; do printf ' %s' "$a"; done >> "$RECORDER_DIR/fake-nft.log"
    printf '\n' >> "$RECORDER_DIR/fake-nft.log"
fi

exit 0
