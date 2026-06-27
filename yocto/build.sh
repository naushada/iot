#!/bin/bash
# build.sh — Containerised multi-arch Yocto build for the iot LwM2M stack
#
# Builds inside a podman/docker container, then copies artifacts to the
# host. No Yocto install needed on the host — just podman or docker.
#
# Usage:
#   ./build.sh                        # raspberrypi3-64 (default)
#   MACHINE=qemuarm64 ./build.sh      # ARM64 / aarch64 (qemu)
#   MACHINE=qemux86-64 ./build.sh     # x86-64 (qemu, CI)
#   ./build.sh all                    # All supported machines
#
# Default target is the full bootable distribution `iot-image`. Override TARGET:
#   TARGET=iot ./build.sh               # just the iot apps recipe → iot-*.ipk
#   TARGET=packagegroup-iot ./build.sh  # the whole .ipk feed (no image)
#   TARGET=iot-bundle ./build.sh        # one .tar.gz of the feed (LwM2M OTA push)
# App-only targets recompile just the iot recipe (deps restore from sstate, so
# minutes not hours) — reach for the full image only on OS/kernel/unit changes.
#
# Guarantee the image carries the latest commit on IOT_BRANCH (main). The iot
# recipe is AUTOREV, but a cached git mirror / sstate can serve a stale checkout
# so a reflash ships old code. Force a fresh re-clone + recompile of just the
# iot recipe (deps still restore from sstate, so it stays fast):
#   IOT_FRESH=1 ./build.sh
#
# Build a non-`main` ref of the iot recipe (e.g. a release branch) — the recipe
# fetches its source from IOT_BRANCH, so the image/feed/bundle carries THAT
# branch's code (combine with IOT_FRESH=1 to force a clean re-fetch):
#   IOT_BRANCH=release/v1.1.0 IOT_FRESH=1 ./build.sh
#
# Build the RAUC A/B (dual-bank) image instead of the single-rootfs one:
#   IOT_AB=1 ./build.sh        # raspberrypi*: u-boot + 4-partition wic +
#                              # signed update-bundle-*.raucb. See
#                              # apps/docs/tdd-ab-image-ota.md.
#
# Output per machine:
#   yocto/build/<machine>/images/<machine>/*.wic.bz2   flashable SD image
#   yocto/build/<machine>/ipk/                          *.ipk feed (opkg)
#
# Requirements:
#   - podman or docker
#   - ~50 GB free disk space (Yocto downloads + build + image)
#   - RAM: 8 GB works but is slow. entrypoint.sh derives BB_NUMBER_THREADS /
#     PARALLEL_MAKE from available RAM (+ bitbake PSI pressure regulation) to
#     avoid OOM-killed compiles (cc1plus on gcc/nodejs-native). More RAM = more
#     parallelism: 8 GB→1 recipe at a time, 14 GB→2, 24 GB→4. Bump the podman VM
#     with `podman machine set --memory <MB>` (machine stopped) for faster builds.
#   - Internet access (first build fetches ~8 GB of sources)
#
# Persistent caches (named volumes, survive across runs → incremental):
#   iot-yocto-downloads   fetched source tarballs
#   iot-yocto-sstate      shared-state cache
# Reset them for a fully clean rebuild:
#   podman volume rm iot-yocto-downloads iot-yocto-sstate
#
# Shareable cache image (skip the whole distro compile on a fresh machine):
#   ./build.sh publish            # after one full build: bake the populated
#                                 # sstate+downloads into $CACHE_IMAGE and push
#   IOT_USE_CACHE=1 ./build.sh    # pull $CACHE_IMAGE and build — only the
#                                 # meta-iot recipes recompile (minutes, not hours)
#
# Bust a stale base-files / image sstate that ships the self-bricking /etc/fstab
# (/dev/sda4, mmcblk0p5/6 → boot hangs / emergency mode). cleansstates base-files
# AND iot-image so the rootfs rebuilds from the clean base-files, then copies the
# image out as usual. Reach for this when a freshly-flashed card hangs on
# /dev/sda4 despite a clean base-files bbappend:
#   IOT_CLEAN=1 ./build.sh        # (combine with IOT_AB=1 for the A/B image)
#
# Interactive build-container shell (ad-hoc bitbake / bitbake-layers, e.g. to
# clear a stale cache or trace where a value comes from):
#   ./build.sh shell              # default machine; then: bitbake -c cleansstate base-files
#   IOT_AB=1 ./build.sh shell raspberrypi3-64   # with the A/B layers added
# A manual bitbake inside `shell` does NOT auto-copy (and the shell is --rm), so
# pull the fresh image out to the host from a second terminal:
#   ./build.sh copy [machine]     # podman cp deploy/ → yocto/build/<machine>/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/build"
IMAGE_NAME="${IMAGE_NAME:-iot-yocto-builder:latest}"
# Published cache image (base builder + baked sstate/downloads mirrors).
CACHE_IMAGE="${CACHE_IMAGE:-docker.io/naushada/iot-yocto-builder:cache}"
SSTATE_VOLUME="iot-yocto-sstate"
DOWNLOADS_VOLUME="iot-yocto-downloads"
# bitbake target — the full bootable distribution by default.
TARGET="${TARGET:-iot-image}"
DEFAULT_MACHINE="${MACHINE:-raspberrypi3-64}"

# Supported machines (used by `./build.sh all`)
MACHINES=(
    "raspberrypi3-64"
    "qemuarm64"
    "qemux86-64"
)

# ── Helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}═══ $1 ═══${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

# ── Detect container runtime ──────────────────────────────────────────
detect_runtime() {
    if command -v podman &>/dev/null; then
        echo "podman"
    elif command -v docker &>/dev/null; then
        echo "docker"
    else
        log_error "Neither podman nor docker found."
        exit 1
    fi
}

# ── Build the container image ─────────────────────────────────────────
build_image() {
    log_section "Building container image: $IMAGE_NAME"
    $CR build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Containerfile" "$SCRIPT_DIR"
    log_info "Image ready: $IMAGE_NAME"
}

# Decide the builder image to run with: pull the published cache image
# (sstate floor → only meta-iot rebuilds) when IOT_USE_CACHE is set,
# otherwise build the base image locally.
prepare_image() {
    if [ -n "${IOT_USE_CACHE:-}" ]; then
        log_section "Pulling cache image: $CACHE_IMAGE"
        # The published cache is an optional speedup, not a hard dependency. If
        # it is missing/expired/unauthenticated (e.g. `manifest unknown`), fall
        # back to building the base image locally instead of aborting the whole
        # build — the same from-scratch path the cache-less builds already use.
        # The `if $CR pull` guard also keeps the failed pull from tripping
        # `set -e`.
        if $CR pull "$CACHE_IMAGE"; then
            IMAGE_NAME="$CACHE_IMAGE"
            log_info "Using baked sstate from $CACHE_IMAGE — only meta-iot recompiles"
        else
            log_info "Cache image $CACHE_IMAGE unavailable — building the base image from scratch"
            build_image
        fi
    else
        build_image
    fi
}

# ── Publish a cache image (base builder + baked sstate/downloads) ──────
# Snapshots the populated volumes into image-layer dirs via a throwaway
# container, commits, and pushes. Run AFTER one full `./build.sh` has
# populated the volumes, and after `podman login docker.io`.
publish() {
    build_image   # ensure the base builder is current

    if ! $CR volume exists "$SSTATE_VOLUME" 2>/dev/null; then
        log_error "Volume $SSTATE_VOLUME is missing — run a full ./build.sh first."
        exit 1
    fi

    local snap="iot-cache-snapshot"
    $CR rm -f "$snap" &>/dev/null || true

    log_section "Baking sstate + downloads into image layers"
    # Mount the populated volumes read-only and copy them into plain overlay
    # dirs (NOT volumes), so `commit` captures them in the image.
    $CR run --name "$snap" \
        -v "${SSTATE_VOLUME}:/mnt/sstate:ro" \
        -v "${DOWNLOADS_VOLUME}:/mnt/dl:ro" \
        --entrypoint bash \
        "$IMAGE_NAME" -c '
            mkdir -p /home/builduser/yocto/sstate-mirror /home/builduser/yocto/dl-mirror
            cp -a /mnt/sstate/. /home/builduser/yocto/sstate-mirror/ 2>/dev/null || true
            cp -a /mnt/dl/.     /home/builduser/yocto/dl-mirror/     2>/dev/null || true
            du -sh /home/builduser/yocto/sstate-mirror /home/builduser/yocto/dl-mirror'

    log_section "Committing cache image: $CACHE_IMAGE"
    # The snapshot ran with --entrypoint bash; restore the base image's
    # runtime config so the cache image behaves like the builder.
    $CR commit \
        --change 'ENTRYPOINT ["/home/builduser/yocto/entrypoint.sh"]' \
        --change 'CMD ["iot-image"]' \
        --change 'USER builduser' \
        --change 'WORKDIR /home/builduser/yocto' \
        "$snap" "$CACHE_IMAGE"
    $CR rm "$snap"

    log_section "Pushing $CACHE_IMAGE"
    $CR push "$CACHE_IMAGE"
    log_info "Published. On any machine: IOT_USE_CACHE=1 ./build.sh"
}

# ── Build one architecture ────────────────────────────────────────────
build_machine() {
    local machine="$1"
    local out="$OUT_DIR/$machine"
    local container="iot-build-${machine//./-}"
    mkdir -p "$out"

    log_section "Building for $machine"

    # Remove any stale container with the same name
    $CR rm -f "$container" &>/dev/null || true

    # Persist the two heavy, reusable Yocto caches on named volumes so the
    # build does NOT balloon the container's ephemeral overlay and so reruns
    # are incremental (sstate restores most tasks instead of rebuilding):
    #   downloads     — fetched source tarballs (arch-independent, shared)
    #   sstate-cache  — shared state cache (hash-keyed, safe to share)
    # The :U flag chowns the volumes to the in-container builduser (uid 1000)
    # — without it a fresh root-owned volume is unwritable by the build user.
    # TMPDIR stays in the overlay but is kept small by rm_work (entrypoint).
    #
    # meta-iot is also bind-mounted (ro) over the copy baked into the image,
    # so edits to recipes take effect on the next run WITHOUT rebuilding the
    # builder image. (The bitbake recipe lives in the image; without this a
    # recipe fix only applies after `podman build` re-COPYs meta-iot.)
    $CR run --name "$container" \
        -e "MACHINE=$machine" \
        -e "IOT_FRESH=${IOT_FRESH:-}" \
        -e "IOT_CLEAN=${IOT_CLEAN:-}" \
        -e "IOT_AB=${IOT_AB:-}" \
        -e "IOT_BRANCH=${IOT_BRANCH:-}" \
        -e "IOT_HASH_LOCAL=1" \
        -v "$SCRIPT_DIR/meta-iot:/home/builduser/yocto/meta-iot:ro" \
        -v "${DOWNLOADS_VOLUME}:/home/builduser/yocto/build/downloads:U" \
        -v "${SSTATE_VOLUME}:/home/builduser/yocto/build/sstate-cache:U" \
        "$IMAGE_NAME" \
        $TARGET

    # Copy artifacts (images/ + ipk/) from the container to the host
    log_info "Copying artifacts to $out ..."
    $CR cp "$container:/home/builduser/yocto/build/tmp/deploy/." "$out/"

    # Clean up the container
    $CR rm "$container"

    log_info "Done: $out/"
}

# ── Print summary ─────────────────────────────────────────────────────
print_summary() {
    log_section "Build summary"
    for machine in "${MACHINES[@]}"; do
        # `|| true`: when only one machine was built the others' dirs don't
        # exist, so find exits non-zero; under `set -euo pipefail` a bare
        # `local x; x=$(failing pipeline)` would abort the script (and thus
        # report failure) right after the successful build. Absorb it.
        local img
        img=$(find "$OUT_DIR/$machine/images" -name 'iot-image*.wic.bz2' -type f 2>/dev/null | sort | tail -1 || true)
        local ipk_count
        ipk_count=$(find "$OUT_DIR/$machine/ipk" -name 'iot-*.ipk' -type f 2>/dev/null | wc -l | tr -d ' ' || true)
        local bundle
        bundle=$(find "$OUT_DIR/$machine/images" -name 'iot-bundle-*.tar.gz' -type f 2>/dev/null | sort | tail -1 || true)
        if [ -n "$img" ]; then
            echo "  ✅ $machine  →  $(basename "$img")  +  ${ipk_count} iot .ipk${bundle:+  +  $(basename "$bundle")}"
        elif [ "$ipk_count" -gt 0 ]; then
            echo "  ⚠️  $machine  →  ${ipk_count} iot .ipk (no image — package-only build)"
        elif [ -d "$OUT_DIR/$machine" ]; then
            echo "  ⚠️  $machine  →  build completed (no artifacts found)"
        else
            echo "  ⬜ $machine  —  not built"
        fi
    done

    # Concrete next steps for the primary RPi target if it was built.
    local rpi_img
    rpi_img=$(find "$OUT_DIR/raspberrypi3-64/images" -name 'iot-image*.wic.bz2' -type f 2>/dev/null | sort | tail -1 || true)
    if [ -n "$rpi_img" ]; then
        cat <<EOF

  ── Flash the SD card (Raspberry Pi 3B) ──────────────────────────────
    # Easiest: flash-sd.sh auto-detects the card, wipes it, writes the newest
    # image, and (optionally) seeds WiFi creds so a CI image joins your AP on
    # first boot. Both --wifi flags are required together:
    ./yocto/flash-sd.sh --wifi-ssid <SSID> --wifi-psk <WPA-PSK>
    ./yocto/flash-sd.sh --list           # just list candidate SD cards
    # (see ./yocto/flash-sd.sh --help for --personalize / zero-touch BS seed)

    # Or flash manually with dd. Find the device first: lsblk (Linux, /dev/sdX)
    # or diskutil list (macOS, /dev/diskN). Write the whole disk, not a partition.
    bzcat "$rpi_img" | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
    # macOS: target the raw node /dev/rdiskN instead of /dev/diskN — much faster.

  ── First boot + ssh in ──────────────────────────────────────────────
    # The image runs sshd with debug-tweaks (empty root password).
    ssh root@<pi-ip>          # then: passwd, provision keys for production

  ── Push iot app updates over ssh (opkg feed) ────────────────────────
    scp $OUT_DIR/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
    ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'

  ── OTA the whole iot-* userspace over LwM2M (one shot, many devices) ─
    # Copy the bundle into the cloud firmware feed + add its manifest row,
    # then push from the cloud-ui Software Update page (multi-select devices).
    scp $OUT_DIR/raspberrypi3-64/images/raspberrypi3-64/iot-bundle-*.tar.gz* <cloud>:/firmware/
    # Paste images/raspberrypi3-64/iot-bundle-*.tar.gz.manifest.json into cloud.firmware.manifest
EOF
    fi
    echo ""
}

# ── Copy artifacts out of a running shell container ───────────────────
# `./build.sh shell` runs --rm with NO auto-copy, so a manual `bitbake` there
# (e.g. `bitbake -c cleansstate base-files iot-image && bitbake iot-image` to
# bust a stale fstab) leaves the fresh image stranded in the container — and the
# container is --rm, so it's lost on exit. `./build.sh copy` pulls deploy/ out to
# the host (same copy a normal build does), to be run from a SECOND terminal
# while the shell is still open. (For an orchestrated clean rebuild that copies
# automatically, prefer `IOT_CLEAN=1 ./build.sh` instead — no shell needed.)
copy_from_shell() {
    local machine="$1"
    local container="iot-shell-${machine//./-}"
    local out="$OUT_DIR/$machine"
    if ! $CR inspect "$container" >/dev/null 2>&1; then
        log_error "No running shell container '$container'."
        log_info  "Start one with:  ./build.sh shell $machine   (then bitbake inside it)"
        log_info  "or just run an auto-copying clean rebuild:  IOT_CLEAN=1 ./build.sh $machine"
        exit 1
    fi
    mkdir -p "$out"
    log_section "Copying artifacts from $container → $out"
    $CR cp "$container:/home/builduser/yocto/build/tmp/deploy/." "$out/"
    local img
    img=$(ls -t "$out"/images/*/iot-image-*.rootfs-*.wic.bz2 2>/dev/null | head -1 || true)
    log_info "Done: $out/"
    [ -n "$img" ] && log_info "Newest image: $img"
}

# Interactive shell in the build container with the full bitbake env ready
# (entrypoint does the layer + local.conf setup, then `exec bash` on IOT_SHELL=1).
# For ad-hoc bitbake / bitbake-layers — e.g. `bitbake -c cleansstate base-files`,
# `bitbake-layers show-layers`, grepping the other layers. Same volumes (sstate +
# downloads) as a build, so changes here affect real builds.
shell_into() {
    local machine="$1"
    local container="iot-shell-${machine//./-}"
    $CR rm -f "$container" >/dev/null 2>&1 || true
    log_section "Build-container shell ($machine)"
    $CR run --rm -it --name "$container" \
        -e "MACHINE=$machine" \
        -e "IOT_AB=${IOT_AB:-}" \
        -e "IOT_BRANCH=${IOT_BRANCH:-}" \
        -e "IOT_HASH_LOCAL=1" \
        -e "IOT_SHELL=1" \
        -v "$SCRIPT_DIR/meta-iot:/home/builduser/yocto/meta-iot:ro" \
        -v "${DOWNLOADS_VOLUME}:/home/builduser/yocto/build/downloads:U" \
        -v "${SSTATE_VOLUME}:/home/builduser/yocto/build/sstate-cache:U" \
        "$IMAGE_NAME"
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
    CR=$(detect_runtime)
    log_info "Container runtime: $CR"

    # Subcommand: publish the cache image (no machine build).
    if [ "${1:-}" = "publish" ]; then
        publish
        return
    fi

    # Subcommand: interactive bitbake shell (no build).
    if [ "${1:-}" = "shell" ]; then
        prepare_image
        shell_into "${2:-$DEFAULT_MACHINE}"
        return
    fi

    # Subcommand: copy artifacts from a running shell container to the host
    # (no build) — for when a manual bitbake inside `./build.sh shell` produced
    # an image that never auto-copied out.
    if [ "${1:-}" = "copy" ]; then
        copy_from_shell "${2:-$DEFAULT_MACHINE}"
        return
    fi

    prepare_image

    log_info "Target recipe: $TARGET"
    local machine="${1:-$DEFAULT_MACHINE}"

    if [ "$machine" = "all" ]; then
        for m in "${MACHINES[@]}"; do
            build_machine "$m"
        done
        print_summary
    else
        build_machine "$machine"
        print_summary
    fi
}

main "$@"
