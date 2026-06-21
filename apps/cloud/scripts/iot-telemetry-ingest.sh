#!/bin/sh
# iot-telemetry-ingest — bridge between the iot-httpd NDJSON spool and Mongo,
# in BOTH directions (§3b of apps/docs/tdd-vehicle-telemetry.md):
#
#   1. INGEST  spool.ndjson ──mongoimport──► Mongo `telemetry`   (write path)
#   2. EXPORT  Mongo `telemetry` ──mongoexport──► history.json    (read path)
#
# Runs in the mongo:5.0 image (has mongoimport + mongoexport); no mongo driver
# is linked into the cloud C++ build. iot-httpd appends snapshots to the spool
# and serves history.json at /api/v1/cloud/telemetry/history. Loops every
# TELEMETRY_INGEST_SEC. Idempotent: an empty spool or empty window is a no-op.

set -u

SPOOL=/var/lib/iot/telemetry-spool/spool.ndjson
HISTORY=/var/lib/iot/telemetry-spool/history.json
HOST="${MONGO_HOST:-mongo}"
WINDOW="${HISTORY_WINDOW_SEC:-86400}"      # read-back window, default 24h
INTERVAL="${TELEMETRY_INGEST_SEC:-30}"

while true; do
    # 1) INGEST — atomically claim the spool so iot-httpd's next append opens a
    #    fresh file (it opens+appends+closes per snapshot, so the rename never
    #    races a held descriptor), then bulk-load and drop the claimed copy.
    if [ -s "$SPOOL" ]; then
        if mv "$SPOOL" "$SPOOL.proc" 2>/dev/null; then
            mongoimport --quiet --host "$HOST" --db iot --collection telemetry \
                --file "$SPOOL.proc" && rm -f "$SPOOL.proc"
        fi
    fi

    # 2) EXPORT — dump the last WINDOW seconds (oldest-first) as a JSON array to
    #    a temp file, then atomically swap it in so iot-httpd never serves a
    #    half-written file. ts is unix-seconds (set by iot-httpd at append).
    cutoff=$(( $(date +%s) - WINDOW ))
    if mongoexport --quiet --host "$HOST" --db iot --collection telemetry \
            --query "{\"ts\":{\"\$gte\":$cutoff}}" --sort "{\"ts\":1}" \
            --jsonArray --out "$HISTORY.tmp" 2>/dev/null; then
        mv "$HISTORY.tmp" "$HISTORY"
    fi

    sleep "$INTERVAL"
done
