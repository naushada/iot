#!/bin/sh
# iot-telemetry-archiver — cold-storage archiver for cloud vehicle telemetry.
#
# Exports telemetry older than ARCHIVE_HOT_DAYS to a self-contained mongodump
# archive, VERIFIES it, and only THEN prunes the archived docs — so nothing is
# deleted before it is safely archived (§3c of apps/docs/tdd-vehicle-telemetry.md).
# A manifest collection maps each archive's date-range → file + sha256 so an
# operator knows which file (HDD) holds which range. Restore with
#   mongorestore --gzip --archive=<file>
#
# Designed for a NORMAL collection + TTL backstop: mongod 5.0 cannot deleteMany
# from a time-series collection, so the ingest pipeline creates `telemetry` as a
# normal collection with a TTL index; this archiver dumps + prunes the aged tail,
# and the TTL only reaps anything the archiver missed.
#
# Run monthly (the iot-archiver compose service loops it). Idempotent: a re-run
# with nothing aged is a no-op.

set -u

MONGO_HOST="${MONGO_HOST:-mongo}"
DB="${ARCHIVE_DB:-iot}"
COLL="${ARCHIVE_COLL:-telemetry}"
HOT_DAYS="${ARCHIVE_HOT_DAYS:-60}"
OUT="${ARCHIVE_DIR:-/archive}"

URI="mongodb://${MONGO_HOST}/${DB}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
CUTOFF="$(date -u -d "-${HOT_DAYS} days" +%Y-%m-%dT%H:%M:%S.000Z)"
ARCHIVE="${OUT}/${COLL}-$(date -u +%Y%m)-${TS}.archive.gz"
QUERY="{\"ts\":{\"\$lt\":{\"\$date\":\"${CUTOFF}\"}}}"

log() { echo "iot-archiver: $*"; }

mkdir -p "${OUT}"
log "archiving ${DB}.${COLL} older than ${CUTOFF}"

# 1. Count candidates — skip cleanly when there is nothing aged.
N="$(mongosh --quiet "${URI}" --eval \
      "db.${COLL}.countDocuments({ts:{\$lt:ISODate(\"${CUTOFF}\")}})" 2>/dev/null)"
case "${N}" in ''|*[!0-9]*) N=0 ;; esac
if [ "${N}" -eq 0 ]; then log "nothing to archive"; exit 0; fi

# 2. Dump (BSON + gzip, lossless + restorable).
if ! mongodump --host "${MONGO_HOST}" --db "${DB}" --collection "${COLL}" \
        --query "${QUERY}" --archive="${ARCHIVE}" --gzip 2>/dev/null; then
    log "FAILED: mongodump"; exit 1
fi

# 3. Verify integrity BEFORE deleting anything.
if ! gzip -t "${ARCHIVE}" 2>/dev/null; then
    log "FAILED: archive integrity (${ARCHIVE}) — NOT pruning"; rm -f "${ARCHIVE}"; exit 1
fi
SHA="$(sha256sum "${ARCHIVE}" | awk '{print $1}')"

# 4. Only now prune the archived docs.
DEL="$(mongosh --quiet "${URI}" --eval \
        "db.${COLL}.deleteMany({ts:{\$lt:ISODate(\"${CUTOFF}\")}}).deletedCount" 2>/dev/null)"
log "archived ${N}, pruned ${DEL:-?} → ${ARCHIVE} (sha256 ${SHA})"

# 5. Append a manifest row (date-range → file + sha) for restore / HDD offload.
mongosh --quiet "${URI}" --eval \
    "db.telemetry_archives.insertOne({file:\"$(basename "${ARCHIVE}")\",to:\"${CUTOFF}\",count:${N},sha256:\"${SHA}\",created:new Date()})" \
    >/dev/null 2>&1 || true

log "done — copy ${OUT} to external HDD; restore with: mongorestore --gzip --archive=<file>"
