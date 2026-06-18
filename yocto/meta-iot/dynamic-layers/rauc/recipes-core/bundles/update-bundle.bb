SUMMARY = "RAUC A/B update bundle (.raucb) for the iot RPi image"
DESCRIPTION = "Signed RAUC bundle carrying the iot-image rootfs for the inactive \
bank. The operator drops this .raucb on the device-ui (chunk-uploaded) or the \
cloud pushes its URL; iot-swupdate runs `rauc install`, the bootloader switches \
banks on reboot, and iot-ota-confirm health-checks + marks the bank good (else \
the bootloader rolls back). See apps/docs/tdd-ab-image-ota.md."
LICENSE = "MIT"

# `bundle` ships in meta-rauc. This recipe lives under dynamic-layers/rauc/ and
# is registered via BBFILES_DYNAMIC (layer.conf), so it's parsed ONLY when
# meta-rauc is layered (an IOT_AB=1 build) — no missing-class risk on a default
# build, which never sees this file.
inherit bundle

# Must match [system] compatible in ../rauc/files/system.conf — rauc
# refuses a bundle whose compatible doesn't match the running system.
RAUC_BUNDLE_COMPATIBLE ?= "iot-rpi"
RAUC_BUNDLE_VERSION    ?= "${PV}"

# Single rootfs slot — the bundle replaces the whole rootfs on the inactive
# bank. The slot image is the full iot-image rootfs as an ext4 (the A/B build
# adds ext4 to IMAGE_FSTYPES; see entrypoint.sh / kas-ab.yml).
RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "iot-image"
RAUC_SLOT_rootfs[fstype] = "ext4"

# Signing material. Dev default = the in-tree dev keypair (self-consistent with
# the dev keyring baked by rauc_%.bbappend). CI/production overrides
# RAUC_KEY_FILE / RAUC_CERT_FILE to the secret prod key + its cert.
FILESEXTRAPATHS:prepend := "${THISDIR}/../rauc/files:"
SRC_URI += " \
    file://dev-ca.cert.pem \
    file://dev-ca.key.pem \
"
RAUC_KEY_FILE  ?= "${WORKDIR}/dev-ca.key.pem"
RAUC_CERT_FILE ?= "${WORKDIR}/dev-ca.cert.pem"
