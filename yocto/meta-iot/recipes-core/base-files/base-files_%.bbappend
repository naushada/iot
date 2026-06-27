# Ship a known-good /etc/fstab so the image never bakes the self-bricking one.
#
# The field failure (apps/docs / fstab_sanity_check in iot-image.bb): a stale
# base-files sstate — a leftover from an abandoned 6-partition layout — injected
# `/dev/mmcblk0p5 -> /data` and `/dev/mmcblk0p6 -> /home`. Those partitions do
# not exist in any current wks (the A/B layout iot-ab.wks.in is p1 boot, p2/p3
# rootA/rootB, p4 data), so the device hangs at boot / drops to emergency mode,
# and on A/B BOTH banks brick before rollback can help.
#
# Prepending our fstab makes it win over any stale/external one AND changes the
# recipe signature, so the bad base-files sstate is invalidated (no cleansstate
# needed). This fstab matches the running A/B device (192.168.1.50): stock
# mounts + /boot from mmcblk0p1. NB the data partition (p4) is intentionally not
# mounted here — same as the live device; see the bbappend note in git for the
# A/B state-persistence follow-up (LABEL=data -> /var/lib/iot).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
