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
PV = "1.3.2"

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
