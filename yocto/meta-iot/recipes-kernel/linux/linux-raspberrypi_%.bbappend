# Kernel config fragment for the on-device container runtime (crun +
# iot-containerd): overlayfs for the container rootfs, the namespaces + cgroup
# controllers for isolation and the memory/CPU caps the run form sets, and
# seccomp for crun's default syscall filter. Most of these are already enabled
# in the raspberrypi3-64 defconfig — the fragment makes the requirement explicit
# and is a no-op where the symbol is already set.
# See apps/docs/tdd-device-containers.md.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://container.cfg"
