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
PURGE=/var/lib/iot/telemetry-spool/purge.list
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

    # 1b) PURGE — a transferred-out device's history is deleted. iot-cloudd has
    #     no mongo driver, so it appends released serials here (one per line) on
    #     transfer-out; we deleteMany({endpoint}) per serial. Claim the file so a
    #     concurrent append starts fresh. Serials are validated to a safe charset
    #     before reaching mongosh (operator-supplied via the release request).
    if [ -s "$PURGE" ]; then
        if mv "$PURGE" "$PURGE.proc" 2>/dev/null; then
            while IFS= read -r serial; do
                case "$serial" in
                    ''|*[!A-Za-z0-9._@-]*) continue ;;   # skip empty / unsafe
                esac
                n=$(mongosh --quiet "mongodb://$HOST/iot" --eval \
                    "db.telemetry.deleteMany({endpoint:\"$serial\"}).deletedCount" 2>/dev/null)
                echo "iot-telemetry-ingest: purged ${n:-?} history doc(s) for $serial"
            done < "$PURGE.proc"
            rm -f "$PURGE.proc"
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
