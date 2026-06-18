# rauc_%.bbappend — install the iot A/B system config + trust keyring.
#
# meta-rauc provides the target `rauc` recipe (the updater daemon/CLI). This
# append ships our slot map (system.conf) and the bundle-signature trust anchor
# (keyring.pem) so `rauc install` verifies bundles and knows the A/B slots.
#
# The keyring here is the DEV CA cert (dev-ca.cert.pem) — it trusts bundles
# signed by the in-tree dev key, so a local IOT_AB=1 build is self-consistent.
# For production, override RAUC_KEYRING_FILE to the prod cert (CI bakes it in),
# matching the prod key kept as a CI secret. See gen-dev-keys.sh.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://system.conf \
    file://dev-ca.cert.pem \
"

# Trust anchor baked into the image. Override for production builds:
#   RAUC_KEYRING_FILE = "prod-ca.cert.pem"
RAUC_KEYRING_FILE ?= "dev-ca.cert.pem"

do_install:append() {
    install -d ${D}${sysconfdir}/rauc
    install -m 0644 ${WORKDIR}/system.conf          ${D}${sysconfdir}/rauc/system.conf
    install -m 0644 ${WORKDIR}/${RAUC_KEYRING_FILE} ${D}${sysconfdir}/rauc/keyring.pem
}

FILES:${PN} += " \
    ${sysconfdir}/rauc/system.conf \
    ${sysconfdir}/rauc/keyring.pem \
"
