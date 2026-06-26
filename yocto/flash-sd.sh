#!/bin/bash
# flash-sd.sh — Format an SD card and flash the iot Yocto image onto it.
#
# Picks up the built wic.bz2 SD-card image, wipes the target card's existing
# partition table ("format"), then writes the image. The .wic image is a
# whole-disk image — it carries its own partition table + boot/root
# filesystems — so the write itself lays those down; a classic mkfs "format"
# would just be overwritten. The format step here therefore clears the old
# partition signatures so nothing stale lingers on a larger card.
#
# Works on Linux and macOS.
#
# The target device is optional: with no device the script auto-detects the
# removable/external SD card (and refuses to guess if zero or several are
# present). Pass a device explicitly to override detection.
#
# Usage:
#   ./flash-sd.sh                        # auto-detect the SD card
#   ./flash-sd.sh /dev/sdX               # Linux  (whole disk, not a partition)
#   ./flash-sd.sh /dev/disk4             # macOS  (script writes the raw node)
#   ./flash-sd.sh --list                 # list candidate removable disks, exit
#   ./flash-sd.sh --yes /dev/sdX         # skip the confirmation prompt
#   ./flash-sd.sh --no-format /dev/sdX   # skip the wipe, just dd the image
#
#   MACHINE=qemuarm64 ./flash-sd.sh /dev/sdX     # pick another machine's image
#   IMAGE=/path/to/custom.wic.bz2 ./flash-sd.sh /dev/sdX
#
# Zero-touch personalisation (apps/docs/tdd-bs-hkdf-zerotouch.md): after writing
# the image, drop a per-unit BS PSK seed onto the FAT `boot` partition (which
# macOS/Windows can mount — the ext4 `data` partition they cannot). On first
# boot iot-ds-seed applies it and the device registers with no manual step.
#
#   ./flash-sd.sh --personalize --master @master.hex --serial 100000003d1f9c2e
#   ./flash-sd.sh --personalize --seed /path/to/bs-seed.json   # pre-built seed
#
#   --master is 64-hex or @/path (the SAME master you wrapped into the cloud
#   with bs-master-wrap). --serial is the device's raw serial (RPi: the
#   /proc/cpuinfo Serial). The master never lands on the card — only the
#   derived key does.
#
# Safety: refuses internal/system disks (override with --force), refuses a card
# too small for the image's on-card layout (the A/B image is ~2.4GB — needs a
# ≥4GB card), shows the image + target, and requires you to type "yes".
# Destructive steps use sudo.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MACHINE="${MACHINE:-raspberrypi3-64}"
IMAGES_DIR="$SCRIPT_DIR/build/$MACHINE/images/$MACHINE"

# ── Helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}═══ $1 ═══${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

# Print the header comment block (everything from line 2 up to the first
# non-comment line), with the leading "# " stripped.
usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; }

# On-card size the image needs, in MiB, summed from the partition sizes in the
# deployed wks (wic writes the expanded .wks next to the image). This is what
# makes the capacity guard adapt automatically when the layout grows — e.g. the
# A/B image's two 1024M banks (100M boot + 2×1024M + 256M data ≈ 2.4GB). Prints
# the total MiB, or returns non-zero when no wks is found (default RPi image —
# its wks lives in meta-raspberrypi, not the deploy dir, so the guard is skipped).
required_mb_from_wks() {
    local wks
    wks="$(find "$IMAGES_DIR" -maxdepth 1 -name '*.wks' -type f 2>/dev/null | sort | tail -1 || true)"
    [ -n "$wks" ] || return 1
    awk '
        /^[[:space:]]*part/ {
            for (i = 1; i <= NF; i++)
                if ($i == "--size" || $i == "--fixed-size") {
                    v = $(i + 1); u = substr(v, length(v), 1); n = v + 0
                    if (u == "G" || u == "g") n *= 1024
                    total += n
                }
        }
        END { if (total > 0) print total; else exit 1 }
    ' "$wks"
}

OS="$(uname -s)"

# ── List removable disks (read-only) ─────────────────────────────────
# macOS: a card in the built-in SD reader is reported with Device Location
# "Internal" even though its media is Removable, so filtering on "external"
# alone misses it. Instead walk every physical disk and keep those that are
# External OR have Removable media — the same rule the write-time check uses.
darwin_sd_disks() {
    diskutil list physical 2>/dev/null \
        | awk '/^\/dev\/disk[0-9]+ \(/{print $1}' \
        | while read -r d; do
            info="$(diskutil info "$d" 2>/dev/null)" || continue
            loc="$(echo "$info" | awk -F: '/Device Location/{print $2; exit}' | xargs)"
            rem="$(echo "$info" | awk -F: '/Removable Media/{print $2; exit}' | xargs)"
            if [ "$loc" = "External" ] || [ "$rem" = "Removable" ]; then
                echo "$d"
            fi
        done
}

list_disks() {
    log_section "Candidate removable disks"
    local out=""
    if [ "$OS" = "Darwin" ]; then
        local d info size name
        while read -r d; do
            [ -n "$d" ] || continue
            info="$(diskutil info "$d" 2>/dev/null || true)"
            size="$(echo "$info" | awk -F: '/Disk Size/{print $2; exit}' | xargs)"
            name="$(echo "$info" | awk -F: '/Device \/ Media Name/{print $2; exit}' | xargs)"
            out+="  $d  ${size}  ${name}"$'\n'
        done < <(darwin_sd_disks)
    else
        # RM=1 → removable. Show size/model so the right one is obvious.
        out="$(lsblk -dno NAME,SIZE,RM,TYPE,MODEL 2>/dev/null \
            | awk '$3==1{printf "  /dev/%s  %s  %s\n",$1,$2,$5}')"
    fi
    if [ -n "$out" ]; then
        printf '%s\n' "${out%$'\n'}"
    else
        log_info "No removable disks detected. Insert the SD card and retry."
    fi
}

# Print the device path(s) of removable/external whole disks, one per line.
detect_sd() {
    if [ "$OS" = "Darwin" ]; then
        darwin_sd_disks
    else
        lsblk -dno NAME,RM,TYPE 2>/dev/null \
            | awk '$2==1 && $3=="disk"{print "/dev/"$1}'
    fi
}

# ── Argument parsing ──────────────────────────────────────────────────
ASSUME_YES=0; DO_LIST=0; FORCE=0; SKIP_FORMAT=0
PERSONALIZE=0; BS_MASTER=""; BS_SERIAL=""; SEED_FILE=""
POSARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)              ASSUME_YES=1 ;;
        -l|--list)             DO_LIST=1 ;;
        --force)               FORCE=1 ;;
        --no-format|--skip-format) SKIP_FORMAT=1 ;;
        --personalize)         PERSONALIZE=1 ;;
        --master)              BS_MASTER="${2:-}"; [ $# -ge 2 ] && shift ;;
        --serial)              BS_SERIAL="${2:-}"; [ $# -ge 2 ] && shift ;;
        --seed)                SEED_FILE="${2:-}"; [ $# -ge 2 ] && shift ;;
        -h|--help)             usage; exit 0 ;;
        -*)                    log_error "unknown option: $1"; usage; exit 1 ;;
        *)                     POSARGS+=("$1") ;;
    esac
    shift
done
set -- ${POSARGS[@]+"${POSARGS[@]}"}

if [ "$DO_LIST" -eq 1 ]; then
    list_disks
    exit 0
fi

# ── Validate personalisation inputs up front (before the destructive flash) ──
PERSONALIZE_TOOL="$SCRIPT_DIR/meta-iot/recipes-iot/lwm2m/files/iot-bs-personalize"
if [ "$PERSONALIZE" -eq 1 ]; then
    if [ -n "$SEED_FILE" ]; then
        [ -f "$SEED_FILE" ] || { log_error "--seed: no such file: $SEED_FILE"; exit 1; }
        SEED_TEXT="$(cat "$SEED_FILE")"
    else
        [ -n "$BS_MASTER" ] && [ -n "$BS_SERIAL" ] || {
            log_error "--personalize needs --master <hex|@file> + --serial <serial>, or --seed <file>."
            exit 1; }
        [ -x "$PERSONALIZE_TOOL" ] || command -v python3 >/dev/null 2>&1 || {
            log_error "iot-bs-personalize needs python3 (not found)."; exit 1; }
        [ -f "$PERSONALIZE_TOOL" ] || { log_error "tool missing: $PERSONALIZE_TOOL"; exit 1; }
        # Derive the seed NOW so a bad master/serial fails before we wipe the card.
        if ! SEED_TEXT="$(python3 "$PERSONALIZE_TOOL" "$BS_MASTER" "$BS_SERIAL")"; then
            log_error "iot-bs-personalize failed (bad master or serial?)."; exit 1
        fi
    fi
fi

# ── Resolve the image ─────────────────────────────────────────────────
if [ -n "${IMAGE:-}" ]; then
    IMG="$IMAGE"
elif [ -e "$IMAGES_DIR/iot-image-$MACHINE.rootfs.wic.bz2" ]; then
    # Stable symlink → newest build.
    IMG="$IMAGES_DIR/iot-image-$MACHINE.rootfs.wic.bz2"
else
    IMG="$(find "$IMAGES_DIR" -name 'iot-image-*.wic.bz2' -type f 2>/dev/null | sort | tail -1 || true)"
fi

if [ -z "${IMG:-}" ] || [ ! -e "$IMG" ]; then
    log_error "No image found for MACHINE=$MACHINE."
    log_info  "Looked in: $IMAGES_DIR"
    log_info  "Build one with:  ./build.sh    (or set IMAGE=/path/to.wic.bz2)"
    exit 1
fi
IMG_SIZE="$(du -hL "$IMG" 2>/dev/null | cut -f1)"

# ── Resolve / validate the target device ──────────────────────────────
# Target device: use the one given, otherwise auto-detect the SD card.
DEV="${1:-}"
if [ -z "$DEV" ]; then
    log_section "Detecting SD card"
    CANDIDATES="$(detect_sd)"
    COUNT="$(printf '%s\n' "$CANDIDATES" | grep -c . || true)"
    if [ "$COUNT" -eq 0 ]; then
        log_error "No removable/external disk detected. Insert the SD card and retry,"
        log_error "or pass the device explicitly, e.g.:  $0 /dev/sdX"
        exit 1
    elif [ "$COUNT" -gt 1 ]; then
        log_error "Multiple removable disks detected — refusing to guess which is the SD card."
        log_error "Re-run with the device explicitly, e.g.:  $0 <device>"
        printf '%s\n' "$CANDIDATES" | sed 's/^/    /' >&2
        exit 1
    fi
    DEV="$(printf '%s\n' "$CANDIDATES" | head -1)"
    log_info "Detected SD card: $DEV"
fi

if [ "$OS" = "Darwin" ]; then
    DEV="/dev/$(basename "$DEV")"                  # normalize diskN → /dev/diskN
    case "$DEV" in
        *s[0-9]*) log_error "Give the whole disk (/dev/diskN), not a partition ($DEV)."; exit 1 ;;
    esac
    [ -e "$DEV" ] || { log_error "No such device: $DEV"; exit 1; }

    INFO="$(diskutil info "$DEV" 2>/dev/null || true)"
    [ -n "$INFO" ] || { log_error "diskutil cannot read $DEV"; exit 1; }
    DEV_SIZE="$(echo "$INFO" | awk -F: '/Disk Size/{print $2; exit}' | xargs)"
    DEV_NAME="$(echo "$INFO" | awk -F: '/Device \/ Media Name/{print $2; exit}' | xargs)"
    # "Disk Size: 31.9 GB (31914983424 Bytes) ..." → the parenthesized byte count.
    DEV_BYTES="$(echo "$INFO" | sed -n 's/.*(\([0-9][0-9]*\) Bytes).*/\1/p' | head -1)"
    # macOS reports e.g. "Device Location: Internal/External" and
    # "Removable Media: Fixed/Removable" (there is no "Internal: Yes" field).
    # A safe target is External OR Removable; an internal fixed disk (the
    # system SSD) is refused unless --force.
    LOCATION="$(echo "$INFO" | awk -F: '/Device Location/{print $2; exit}' | xargs)"
    REMOVABLE_MEDIA="$(echo "$INFO" | awk -F: '/Removable Media/{print $2; exit}' | xargs)"
    if [ "$LOCATION" != "External" ] && [ "$REMOVABLE_MEDIA" != "Removable" ]; then
        if [ "$FORCE" -ne 1 ]; then
            log_error "$DEV is not external/removable (Location=${LOCATION:-?}, Media=${REMOVABLE_MEDIA:-?}): $DEV_NAME"
            log_error "Refusing to avoid wiping a system/internal disk. Override with --force only if certain."
            exit 1
        fi
        log_info "$DEV is internal/fixed — proceeding anyway (--force)."
    fi
    TARGET="/dev/r$(basename "$DEV")"              # raw node → much faster writes
    BS="4m"
    UNMOUNT=(diskutil unmountDisk "$DEV")
    EJECT=(diskutil eject "$DEV")
else
    case "$DEV" in /dev/*) ;; *) DEV="/dev/$DEV" ;; esac
    [ -b "$DEV" ] || { log_error "Not a block device: $DEV"; exit 1; }
    TYPE="$(lsblk -dno TYPE "$DEV" 2>/dev/null || true)"
    if [ "$TYPE" != "disk" ]; then
        log_error "$DEV is not a whole disk (type=$TYPE). Give e.g. /dev/sdb, not /dev/sdb1."
        exit 1
    fi
    BASE="$(lsblk -dno NAME "$DEV" 2>/dev/null)"
    REMOVABLE="$(cat "/sys/block/$BASE/removable" 2>/dev/null || echo 0)"
    DEV_SIZE="$(lsblk -dno SIZE "$DEV" 2>/dev/null | xargs)"
    DEV_NAME="$(lsblk -dno MODEL "$DEV" 2>/dev/null | xargs)"
    DEV_BYTES="$(lsblk -bdno SIZE "$DEV" 2>/dev/null | xargs)"   # -b → bytes
    # Refuse the disk that carries the running root filesystem. PKNAME is the
    # parent (whole-disk) kernel name of the partition mounted at /.
    ROOT_SRC="$(findmnt -no SOURCE / 2>/dev/null || true)"
    if [ -n "$ROOT_SRC" ]; then
        ROOT_DISK="$(lsblk -no PKNAME "$ROOT_SRC" 2>/dev/null | head -1 || true)"
        if [ -n "$ROOT_DISK" ] && [ "/dev/$ROOT_DISK" = "$DEV" ]; then
            log_error "$DEV hosts the running root filesystem (/). Refusing."
            exit 1
        fi
    fi
    if [ "$REMOVABLE" != "1" ] && [ "$FORCE" -ne 1 ]; then
        log_error "$DEV is not flagged removable ($DEV_NAME). Refusing. Use --force to override."
        exit 1
    fi
    TARGET="$DEV"
    BS="4M"
    EJECT=(eject "$DEV")
fi

_umount_linux() { for p in "$DEV"?*; do sudo umount "$p" 2>/dev/null || true; done; }

# ── Capacity guard ────────────────────────────────────────────────────
# The wic image is a whole-disk image of a FIXED size (the wks layout total);
# dd onto a smaller card runs out of space mid-write and leaves a truncated,
# unbootable image (often only after the last partition — silent corruption).
# Refuse up front when the card is too small for the layout. Skipped when the
# size can't be determined (no deployed wks, or unparseable device size) so the
# default RPi image and exotic devices still flash. ~3% margin for the partition
# table + alignment slack.
REQ_MB="$(required_mb_from_wks || true)"
if [ -n "${REQ_MB:-}" ] && [ -n "${DEV_BYTES:-}" ] 2>/dev/null && [ "${DEV_BYTES:-0}" -gt 0 ] 2>/dev/null; then
    REQ_BYTES=$(( REQ_MB * 1024 * 1024 ))
    REQ_BYTES=$(( REQ_BYTES + REQ_BYTES / 32 ))     # +~3% headroom
    if [ "$DEV_BYTES" -lt "$REQ_BYTES" ]; then
        log_error "Target $DEV (${DEV_SIZE:-$((DEV_BYTES/1024/1024))MB}) is too small for this image:"
        log_error "the layout needs ~$(( REQ_BYTES / 1024 / 1024 ))MB on-card. Use a larger SD card."
        if [ "$FORCE" -ne 1 ]; then
            log_error "(Override with --force only if you are certain the card is big enough.)"
            exit 1
        fi
        log_info "Proceeding anyway (--force) — the write will fail if it truly doesn't fit."
    fi
fi

# ── Confirm ───────────────────────────────────────────────────────────
log_section "Flash plan"
echo "  Image  : $IMG"
echo "           ($IMG_SIZE compressed)"
echo "  Machine: $MACHINE"
echo "  Target : $DEV  ${DEV_SIZE:+($DEV_SIZE${DEV_NAME:+, $DEV_NAME})}"
[ -n "${REQ_MB:-}" ] && echo "  Layout : ~${REQ_MB}MB on-card (whole-disk wic image)"
echo "  Steps  : unmount → $([ "$SKIP_FORMAT" -eq 1 ] && echo '(skip format)' || echo 'wipe partition table') → write image →$([ "$PERSONALIZE" -eq 1 ] && echo ' personalise (boot partition) →') sync/eject"
[ "$PERSONALIZE" -eq 1 ] && echo "  Seed   : zero-touch BS PSK → boot partition${BS_SERIAL:+ (serial $BS_SERIAL)}"
echo ""
echo -e "${RED}  ⚠  ALL DATA ON $DEV WILL BE DESTROYED.${NC}"

if [ "$ASSUME_YES" -ne 1 ]; then
    printf "  Type 'yes' to continue: "
    ANSWER=""
    if ! read -r ANSWER < /dev/tty 2>/dev/null; then
        echo ""
        log_error "No terminal for confirmation. Re-run in an interactive shell, or pass --yes."
        exit 1
    fi
    if [ "$ANSWER" != "yes" ]; then
        log_info "Aborted."
        exit 1
    fi
fi

# ── 1. Unmount ────────────────────────────────────────────────────────
log_section "Unmounting $DEV"
if [ "$OS" = "Darwin" ]; then
    "${UNMOUNT[@]}" || true
else
    _umount_linux
fi

# ── 2. Format (clear the existing partition table) ────────────────────
if [ "$SKIP_FORMAT" -eq 1 ]; then
    log_info "Skipping format (--no-format)."
else
    log_section "Formatting (wiping partition table) on $DEV"
    if [ "$OS" != "Darwin" ] && command -v wipefs >/dev/null 2>&1; then
        sudo wipefs -a "$DEV" || true
    fi
    # Zero the first 16 MiB to clear MBR/GPT + any leftover superblocks.
    # Write the RAW node ($TARGET, /dev/rdiskN on macOS): dd to the buffered
    # whole-disk node (/dev/diskN) is rejected by macOS with "Operation not
    # permitted". On Linux $TARGET == $DEV, so this is correct on both.
    sudo dd if=/dev/zero of="$TARGET" bs="$BS" count=4 conv=fsync 2>/dev/null \
        || sudo dd if=/dev/zero of="$TARGET" bs="$BS" count=4
    sync
    log_info "Partition table cleared."
fi

# ── 3. Write the image ────────────────────────────────────────────────
log_section "Writing image to $DEV (this can take several minutes)"
if command -v pv >/dev/null 2>&1; then
    bzcat "$IMG" | pv | sudo dd of="$TARGET" bs="$BS"
elif [ "$OS" != "Darwin" ]; then
    bzcat "$IMG" | sudo dd of="$TARGET" bs="$BS" status=progress conv=fsync
else
    # BSD dd has no status=progress and pv isn't installed. Auto-print progress
    # by sending SIGINFO to dd every few seconds (exactly what Ctrl-T does) from
    # a background ticker. The write pipeline stays in the FOREGROUND so
    # set -e/pipefail still abort on a failed bzcat (no silent truncation).
    log_info "No 'pv'; auto-printing dd progress every 5s (brew install pv for a bar)."
    (
        # Wait for dd to start (sudo may prompt first), then tick until it exits.
        while ! pgrep -f "dd of=$TARGET" >/dev/null 2>&1; do sleep 1; done
        while pgrep -f "dd of=$TARGET" >/dev/null 2>&1; do
            sudo pkill -INFO -f "dd of=$TARGET" 2>/dev/null || true
            sleep 5
        done
    ) &
    progress_ticker=$!
    bzcat "$IMG" | sudo dd of="$TARGET" bs="$BS"
    kill "$progress_ticker" 2>/dev/null || true
fi

# ── 3.5 Personalise (zero-touch BS PSK → FAT boot partition) ──────────
# The wic image just laid down a fresh partition table; mount the FAT `boot`
# partition (p1 — the only one macOS/Windows can mount) and drop bs-seed.json.
# iot-ds-seed reads /boot/bs-seed.json on first boot, applies it, and shreds it.
if [ "$PERSONALIZE" -eq 1 ]; then
    log_section "Personalising (BS PSK seed → boot partition)"
    sync
    BOOT_MNT=""
    if [ "$OS" = "Darwin" ]; then
        # Re-read the new table and mount p1 (diskNs1). It often auto-mounts.
        diskutil mountDisk "$DEV" >/dev/null 2>&1 || true
        diskutil mount "${DEV}s1" >/dev/null 2>&1 || true
        BOOT_MNT="$(diskutil info "${DEV}s1" 2>/dev/null \
                    | awk -F: '/Mount Point/{sub(/^[ \t]+/,"",$2); print $2; exit}')"
        WRITE=(tee)                              # /Volumes/boot is user-writable
        UNMOUNT_BOOT=(diskutil unmount "${DEV}s1")
    else
        sudo partprobe "$DEV" 2>/dev/null || true
        # The FAT partition by label, robust across sdb1 vs mmcblk0p1.
        BOOTPART="$(lsblk -rno NAME,LABEL "$DEV" 2>/dev/null | awk '$2=="boot"{print "/dev/"$1; exit}')"
        [ -n "$BOOTPART" ] || BOOTPART="$(lsblk -rno NAME "$DEV" 2>/dev/null | sed -n '2p' | sed 's|^|/dev/|')"
        BOOT_MNT="$(mktemp -d)"
        sudo mount "$BOOTPART" "$BOOT_MNT" 2>/dev/null || BOOT_MNT=""
        WRITE=(sudo tee)
        UNMOUNT_BOOT=(sudo umount "$BOOTPART")
    fi
    if [ -z "$BOOT_MNT" ] || [ ! -d "$BOOT_MNT" ]; then
        log_error "Could not mount the boot partition to write the seed."
        log_error "Image is flashed; personalise manually: copy a bs-seed.json"
        log_error "(from iot-bs-personalize) onto the card's boot partition."
    else
        printf '%s\n' "$SEED_TEXT" | "${WRITE[@]}" "$BOOT_MNT/bs-seed.json" >/dev/null
        sync
        "${UNMOUNT_BOOT[@]}" >/dev/null 2>&1 || true
        [ "$OS" = "Darwin" ] || rmdir "$BOOT_MNT" 2>/dev/null || true
        log_info "Seeded bs-seed.json onto the boot partition${BS_SERIAL:+ (serial $BS_SERIAL)}."
    fi
fi

# ── 4. Flush + eject ──────────────────────────────────────────────────
log_section "Flushing"
sync
"${EJECT[@]}" 2>/dev/null || true

log_info "Done. $DEV is flashed and safe to remove."
if [ "$PERSONALIZE" -eq 1 ]; then
    echo ""
    echo "  Zero-touch: the device applies bs-seed.json on first boot. Ensure the"
    echo "  cloud has the matching master+KEK (cloud.bs.master.key + IOT_BS_MASTER_KEK)."
fi
echo ""
echo "  Boot the Pi, then ssh in (sshd is baked in; debug-tweaks → empty root pw):"
echo "    ssh root@<pi-ip>"
if [ -n "${REQ_MB:-}" ]; then
    # A deployed wks in the image dir → this is the RAUC A/B image.
    echo ""
    echo "  A/B image — verify the dual-bank layout and OTA readiness on-target:"
    echo "    lsblk            # expect boot / rootA / rootB / data"
    echo "    rauc status      # two rootfs slots, one booted + good"
    echo "    rauc install /path/to/update-bundle-*.raucb   # writes the inactive bank"
fi
