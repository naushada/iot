SUMMARY = "IoT LwM2M gateway distribution — full stack, SSH, RPi drivers"
DESCRIPTION = "Bootable Yocto image for the iot LwM2M 1.1.1 gateway stack. \
Bundles packagegroup-iot-full (ds-server, lwm2m, ds-cli, iot-httpd, \
openvpn-client, net-router, wifi-client) plus the kernel modules and \
firmware needed for Raspberry Pi 3B onboard Wi-Fi/Bluetooth, an SSH server \
(the 'install via ssh' entry point), and opkg package management so the \
iot-*.ipk feed can be pushed and installed over ssh after first boot."
LICENSE = "MIT"

inherit core-image

# ── Image features ─────────────────────────────────────────────────
# ssh-server-openssh : sshd on the running target — you ssh in to operate
#                      it and to opkg-install .ipk updates.
# package-management : ships opkg + the ipk feed config on-target so
#                      `opkg install iot-*.ipk` works over ssh.
IMAGE_FEATURES += "ssh-server-openssh package-management"

# debug-tweaks: empty root password so the very first ssh login works out
# of the box. REMOVE for production — provision an ssh key or set a
# password instead (see DEPLOY notes in build.sh summary).
EXTRA_IMAGE_FEATURES ?= "debug-tweaks"

# ── Payload ────────────────────────────────────────────────────────
# packagegroup-iot-full pulls the whole gateway daemon set + runtime deps
# (openvpn, nftables, iproute2, wpa-supplicant, wireless-tools).
# kernel-modules drags in every built module so USB/net/etc. drivers are
# present ("all required drivers"). The bcm43430 firmware + pi-bluetooth
# light up the Pi 3B's onboard SDIO Wi-Fi and UART Bluetooth.
# linux-firmware-rpidistro-bcm43430 is the Pi-specific firmware+nvram for
# the 3B's BCM43430 Wi-Fi (the upstream linux-firmware blob lacks the RPi
# nvram). It's gated by the synaptics-killswitch LICENSE_FLAG, accepted in
# entrypoint.sh / kas-iot.yml. If your meta-raspberrypi revision names it
# differently, swap to linux-firmware-bcm43430.
IMAGE_INSTALL:append = " \
    packagegroup-iot-full \
    kernel-modules \
    linux-firmware-rpidistro-bcm43430 \
    pi-bluetooth \
    bluez5 \
    openssh \
    opkg \
    kernel-module-tun \
    htop \
    nano \
"

# Headroom on the rootfs for opkg-installed .ipk updates + logs (512 MB).
IMAGE_ROOTFS_EXTRA_SPACE = "524288"

# ── Output artefacts ───────────────────────────────────────────────
# A bzip2'd wic SD-card image plus a .bmap for fast/safe bmaptool writes.
# Flash with:  bmaptool copy iot-image-*.wic.bz2 /dev/sdX
#         or:  bzcat iot-image-*.wic.bz2 | sudo dd of=/dev/sdX bs=4M conv=fsync
IMAGE_FSTYPES = "wic.bz2 wic.bmap"
