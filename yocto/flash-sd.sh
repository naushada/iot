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
# Safety: refuses internal/system disks (override with --force), shows the
# image + target, and requires you to type "yes". Destructive steps use sudo.

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

OS="$(uname -s)"

# ── List removable disks (read-only) ─────────────────────────────────
list_disks() {
    log_section "Candidate removable disks"
    local out
    if [ "$OS" = "Darwin" ]; then
        out="$(diskutil list external physical 2>/dev/null || true)"
    else
        # RM=1 → removable. Show size/model so the right one is obvious.
        out="$(lsblk -dno NAME,SIZE,RM,TYPE,MODEL 2>/dev/null \
            | awk '$3==1{printf "  /dev/%s  %s  %s\n",$1,$2,$5}')"
    fi
    if [ -n "$out" ]; then
        echo "$out"
    else
        log_info "No removable disks detected. Insert the SD card and retry."
    fi
}

# Print the device path(s) of removable/external whole disks, one per line.
detect_sd() {
    if [ "$OS" = "Darwin" ]; then
        # Section headers look like: "/dev/disk4 (external, physical):"
        diskutil list external physical 2>/dev/null \
            | awk '/^\/dev\/disk[0-9]+ \(external/{print $1}'
    else
        lsblk -dno NAME,RM,TYPE 2>/dev/null \
            | awk '$2==1 && $3=="disk"{print "/dev/"$1}'
    fi
}

# ── Argument parsing ──────────────────────────────────────────────────
ASSUME_YES=0; DO_LIST=0; FORCE=0; SKIP_FORMAT=0
POSARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)              ASSUME_YES=1 ;;
        -l|--list)             DO_LIST=1 ;;
        --force)               FORCE=1 ;;
        --no-format|--skip-format) SKIP_FORMAT=1 ;;
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

# ── Confirm ───────────────────────────────────────────────────────────
log_section "Flash plan"
echo "  Image  : $IMG"
echo "           ($IMG_SIZE compressed)"
echo "  Machine: $MACHINE"
echo "  Target : $DEV  ${DEV_SIZE:+($DEV_SIZE${DEV_NAME:+, $DEV_NAME})}"
echo "  Steps  : unmount → $([ "$SKIP_FORMAT" -eq 1 ] && echo '(skip format)' || echo 'wipe partition table') → write image → sync/eject"
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
    log_info "No 'pv' for a progress bar — press Ctrl-T to see dd status."
    bzcat "$IMG" | sudo dd of="$TARGET" bs="$BS"
fi

# ── 4. Flush + eject ──────────────────────────────────────────────────
log_section "Flushing"
sync
"${EJECT[@]}" 2>/dev/null || true

log_info "Done. $DEV is flashed and safe to remove."
echo ""
echo "  Boot the Pi, then ssh in (sshd is baked in; debug-tweaks → empty root pw):"
echo "    ssh root@<pi-ip>"
