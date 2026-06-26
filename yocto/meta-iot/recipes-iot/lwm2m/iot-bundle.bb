SUMMARY = "Single-shot OTA bundle of every iot-*.ipk (LwM2M Object 5)"
DESCRIPTION = "Packs the whole iot-* .ipk feed into one iot-bundle-<ver>.tar.gz \
so the cloud can upgrade a device's full userspace in ONE LwM2M Firmware \
Update push instead of one package at a time. The device's iot-ota-stage \
downloads + sha256-verifies the tarball, extracts the .ipk files into the \
update spool, and iot-swupdate's existing `opkg install *.ipk` glob applies \
them all. Lighter than the RAUC A/B full-image bundle (update-bundle.bb): \
userspace only, no reboot unless a base/kernel package lands."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Keep the leading semver in sync with the repo-root /VERSION (same source of
# truth as iot_git.bb's PV and the baked-in iot.version).
PV = "1.3.3"

# No source to fetch/compile — this recipe only repackages the already-built
# feed in do_deploy. nopackages skips the empty packaging tasks; deploy gives
# us do_deploy + DEPLOYDIR (sstate-copied into DEPLOY_DIR_IMAGE).
inherit nopackages deploy

# The bundle contents (which iot-* packages exist) are machine-specific, so the
# artifact is too — keep it out of the shared/allarch sstate bucket.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# Every iot-*.ipk comes from the single `iot` recipe (split via FILES:${PN}-*),
# so one task dependency guarantees the whole feed is written before we bundle.
do_deploy[depends] += "iot:do_package_write_ipk"

# Runtime shared-lib deps the iot-* binaries link but an OLDER on-device image may
# not already carry. Incremental .ipk OTA installs only what's in the bundle, so a
# NEW or version-bumped lib dep makes `opkg install` fail on a stale device with
# "nothing provides <lib> >= <ver>" — observed 1.3.0→1.3.2: iot-lwm2m needs
# libsqlite3-0>=3.45.3 (the SQLite DurableSampleBuffer) and iot-mqtt needs
# libmosquitto1>=2.0.22, neither on the 1.3.0 image. Bundle them so the OTA is
# self-contained. Bare package basenames; the newest matching .ipk is included.
# Build-deps that drag these in so their .ipk is written to the feed before bundling.
IOT_BUNDLE_EXTRA_PKGS ?= "libsqlite3-0 libmosquitto1"
do_deploy[depends] += "sqlite3:do_package_write_ipk mosquitto:do_package_write_ipk"

do_deploy() {
    feed="${DEPLOY_DIR_IPK}"
    stage="${WORKDIR}/bundle"
    rm -rf "${stage}"
    mkdir -p "${stage}"

    # The deploy feed accumulates every version ever built; pick the NEWEST
    # .ipk per package basename (the part before the first '_') so the bundle
    # never ships two versions of the same package.
    seen=""
    for f in $(find "${feed}" -name 'iot-*.ipk' -type f -printf '%T@ %p\n' \
                 | sort -rn | awk '{print $2}'); do
        base="$(basename "${f}")"
        pkg="${base%%_*}"
        case " ${seen} " in *" ${pkg} "*) continue ;; esac
        seen="${seen} ${pkg}"
        cp -f "${f}" "${stage}/"
    done

    if [ -z "$(ls -A "${stage}" 2>/dev/null)" ]; then
        bbfatal "iot-bundle: no iot-*.ipk found under ${feed} — is the 'iot' recipe built?"
    fi

    # Add the runtime-lib deps (newest each) so a stale device that lacks them can
    # still satisfy opkg. opkg ignores an already-current/newer lib, so including
    # them is harmless when the device is already up to date.
    for pkg in ${IOT_BUNDLE_EXTRA_PKGS}; do
        dep="$(find "${feed}" -name "${pkg}_*.ipk" -type f -printf '%T@ %p\n' \
                 | sort -rn | awk 'NR==1{print $2}')"
        if [ -n "${dep}" ]; then
            cp -f "${dep}" "${stage}/"
            bbnote "iot-bundle: included dep $(basename "${dep}")"
        else
            bbwarn "iot-bundle: dep ${pkg} not found in ${feed} — stale devices may fail opkg"
        fi
    done

    name="iot-bundle-${PV}-${MACHINE}.tar.gz"
    # Archive root holds the flat .ipk files (extracted straight into the spool
    # by iot-ota-stage). Sorted + no mtime/owner noise → reproducible-ish.
    tar --numeric-owner --owner=0 --group=0 -C "${stage}" -czf "${DEPLOYDIR}/${name}" .

    sha="$(sha256sum "${DEPLOYDIR}/${name}" | awk '{print $1}')"
    echo "${sha}  ${name}" > "${DEPLOYDIR}/${name}.sha256"

    # Paste-ready row for the cloud's cloud.firmware.manifest ds key. The cloud
    # serves the bundle from /firmware/<name>; the operator copies the tarball
    # into the iot-firmware volume and appends this object to the manifest array.
    cat > "${DEPLOYDIR}/${name}.manifest.json" <<EOF
{ "pkg": "iot-bundle", "version": "${PV}", "arch": "${MACHINE}",
  "ipk_url": "/firmware/${name}", "sha256": "${sha}" }
EOF

    bbnote "iot-bundle: packed $(ls -1 "${stage}" | wc -l) iot-*.ipk into ${name} (sha256 ${sha})"
}

addtask deploy before do_build after do_compile
