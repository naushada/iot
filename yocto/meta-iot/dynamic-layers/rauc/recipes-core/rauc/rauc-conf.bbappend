# rauc-conf.bbappend — provide the iot A/B system.conf + dev keyring.
#
# meta-rauc's *rauc-conf* recipe (rauc-conf.bb) is the SEPARATE recipe that owns
# /etc/rauc/system.conf and the bundle-verification keyring (it ships a "please
# overwrite" example system.conf). This is the correct place to supply ours.
#
# Why this file exists: the iot config used to be installed from rauc_%.bbappend
# (the rauc *daemon* recipe). But rauc-conf is a different recipe that ALSO ships
# /etc/rauc/system.conf, so the same file landed in two ipks (rauc + rauc-conf)
# and opkg aborted do_rootfs with a file clash ("already provided by package
# rauc-conf"). Overriding it here — rauc-conf's own FILESEXTRAPATHS hook — keeps
# it in exactly one package. See apps/docs/tdd-ab-image-ota.md.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# rauc-conf.bb's SRC_URI already lists `file://system.conf`; the prepended path
# makes OUR files/system.conf (the iot slot map) win over meta-rauc's example.

# Bundle-signature trust anchor = the in-tree dev CA cert (trusts bundles signed
# by the dev key; the matching key is a CI secret for production). meta-rauc
# installs the keyring under its source filename, but our system.conf references
# a stable /etc/rauc/keyring.pem — so rename it after install. For production,
# override RAUC_KEYRING_FILE to the prod cert; the on-target path stays
# keyring.pem, so system.conf needs no change.
RAUC_KEYRING_FILE = "dev-ca.cert.pem"

do_install:append() {
    if [ "${RAUC_KEYRING_FILE}" != "keyring.pem" ] \
        && [ -f ${D}${sysconfdir}/rauc/${RAUC_KEYRING_FILE} ]; then
        mv ${D}${sysconfdir}/rauc/${RAUC_KEYRING_FILE} \
           ${D}${sysconfdir}/rauc/keyring.pem
    fi
}
