#!/bin/bash
# Make a stand-in for a Raspberry Pi OS image from any root file system, to
# test build-image.sh without downloading Raspberry Pi OS: a 512 MB FAT boot
# partition with a stock-style config.txt/cmdline.txt, and an ext4 root.
#
#   sudo ./tools/image/test/make-test-image.sh ROOTFS_DIR_OR_TARBALL OUT.img
set -euo pipefail
SRC=$1 OUT=$2
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
tmp=$(mktemp -d)
lb='' lr=''
done_() {
    set +e
    umount -q "$tmp/m/boot/firmware" 2>/dev/null; umount -q "$tmp/m" 2>/dev/null
    for l in $lb $lr; do losetup -d "$l"; done
    rm -rf "$tmp"
}
trap done_ EXIT
mkdir -p "$tmp/m"
if [ -d "$SRC" ]; then used=$(du -sm "$SRC" | cut -f1); else used=$(( $(stat -c %s "$SRC") * 4 / 1048576 )); fi
root_mb=$(( used + used / 3 + 200 ))
truncate -s 0 "$OUT"; truncate -s $(( 4 + 512 + root_mb ))M "$OUT"
sfdisk -q "$OUT" <<PT
label: dos
label-id: 0x5a45524f
start=8192, size=1048576, type=c
start=1056768, type=83
PT
start1=$(( 8192 * 512 )); size1=$(( 1048576 * 512 )); start2=$(( 1056768 * 512 ))
lb=$(losetup -f --show -o $start1 --sizelimit $size1 "$OUT")
lr=$(losetup -f --show -o $start2 "$OUT")
if grep -qw vfat /proc/filesystems; then
    mkfs.vfat -n BOOTFS "$lb" >/dev/null
else
    # Some CI kernels (and containers) have no FAT driver; the builder doesn't
    # care what the boot partition is formatted with, so use ext4 there.
    echo "note: no vfat in this kernel, boot partition is ext4 (test only)"
    mkfs.ext4 -q -L bootfs "$lb"
fi
mkfs.ext4 -q -L rootfs "$lr"
mount "$lr" "$tmp/m"
if [ -d "$SRC" ]; then cp -a "$SRC"/. "$tmp/m"/; else tar -xpf "$SRC" -C "$tmp/m"; fi
mkdir -p "$tmp/m/boot/firmware"
mount "$lb" "$tmp/m/boot/firmware"
cat > "$tmp/m/boot/firmware/config.txt" <<'CFG'
# For more options and information see
# http://rptl.io/configtxt

# Uncomment some or all of these to enable the optional hardware interfaces
#dtparam=i2c_arm=on
#dtparam=i2s=on
#dtparam=spi=on

# Enable audio (loads snd_bcm2835)
dtparam=audio=on

# Automatically load overlays for detected cameras
camera_auto_detect=1

# Automatically load overlays for detected DSI displays
display_auto_detect=1

# Automatically load initramfs files, if found
auto_initramfs=1

# Enable DRM VC4 V3D driver
dtoverlay=vc4-kms-v3d
max_framebuffers=2
disable_fw_kms_setup=1
arm_64bit=1
disable_overscan=1
arm_boost=1

[cm4]
otg_mode=1

[cm5]
dtoverlay=dwc2,dr_mode=host

[all]
CFG
echo "console=serial0,115200 console=tty1 root=PARTUUID=5a45524f-02 rootfstype=ext4 fsck.repair=yes rootwait quiet splash plymouth.ignore-serial-consoles" \
    > "$tmp/m/boot/firmware/cmdline.txt"
mkdir -p "$tmp/m/boot/firmware/overlays"
printf 'proc /proc proc defaults 0 0\nPARTUUID=5a45524f-01 /boot/firmware vfat defaults 0 2\nPARTUUID=5a45524f-02 / ext4 defaults,noatime 0 1\n' > "$tmp/m/etc/fstab"
echo "test image: $OUT ($(du -h --apparent-size "$OUT" | cut -f1))"
