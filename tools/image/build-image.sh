#!/bin/bash
# Build a flashable ZeroOS SD-card image for the CardputerZero.
#
#   sudo ./tools/image/build-image.sh [options] [-- zeroos-setup options]
#
# Takes a Raspberry Pi OS Lite (arm64) image, installs everything ZeroOS
# needs into it with tools/image/zeroos-setup.sh (run in a chroot), shrinks
# it and compresses it. Flash the result with Raspberry Pi Imager
# ("Use custom") or balenaEtcher.
#
# Options:
#   --base URL|FILE   image to start from (.img, .img.xz, .zip)
#                     [https://downloads.raspberrypi.com/raspios_lite_arm64_latest]
#   --out DIR         where the image goes [./out]
#   --name NAME       file name stem [zeroos-<version>-<date>]
#   --grow MB         room added for the build [4000]
#   --no-compress     leave a plain .img
#   --keep            keep the uncompressed working image in DIR/work
#
# Everything after "--" goes to zeroos-setup.sh, for example
#   -- --user zero --password s3cret --hostname zero1 --region EU_868
# (run tools/image/zeroos-setup.sh --help for the list).
#
# Behind a TLS-inspecting proxy, set ZOS_BUILD_CA=/path/to/ca-bundle.pem.
#
# Needs: Linux, root, losetup, sfdisk, e2fsprogs, xz, curl, rsync, git.
# On an x86-64 machine it also needs qemu-user-static with binfmt_misc
# (Debian/Ubuntu: apt install qemu-user-static binfmt-support). On an arm64
# machine (a Raspberry Pi 4/5, an arm64 cloud runner) it runs natively and
# is several times faster. tools/image/build-docker.sh wraps all of this.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE=https://downloads.raspberrypi.com/raspios_lite_arm64_latest
OUT=$ROOT/out
NAME=
GROW=4000
COMPRESS=1
KEEP=0
SETUP_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE=$2; shift ;;
        --out) OUT=$2; shift ;;
        --name) NAME=$2; shift ;;
        --grow) GROW=$2; shift ;;
        --no-compress) COMPRESS=0 ;;
        --keep) KEEP=1 ;;
        --) shift; SETUP_ARGS=("$@"); break ;;
        -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (zeroos-setup options go after --)" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n\033[1;36m### %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { echo "build-image: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo): it loop-mounts the image"
for t in losetup sfdisk e2fsck resize2fs xz curl rsync git chroot; do
    command -v "$t" >/dev/null || die "missing tool: $t"
done

WORK=$OUT/work
CACHE=$OUT/cache
MNT=$WORK/mnt
IMG=$WORK/zeroos.img
mkdir -p "$WORK" "$CACHE"
# A release build is named after its release (ZOS_RELEASE=v0.3.4, set by CI).
VERSION=${ZOS_RELEASE:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}
[ -n "$NAME" ] || NAME="zeroos-$VERSION-$(date -u +%Y%m%d)"

# ---------------------------------------------------------------------------
LOOP_BOOT=
LOOP_ROOT=
BOOTDIR=/boot/firmware
STASH=$WORK/stash
cleanup() {
    set +e
    if mountpoint -q "$MNT"; then
        # Undo the chroot preparations so the image boots like it should.
        [ -e "$MNT/usr/sbin/policy-rc.d.zos" ] && rm -f "$MNT/usr/sbin/policy-rc.d" &&
            rm -f "$MNT/usr/sbin/policy-rc.d.zos"
        if [ -e "$STASH/resolv.conf" ] || [ -L "$STASH/resolv.conf" ]; then
            rm -f "$MNT/etc/resolv.conf"; mv "$STASH/resolv.conf" "$MNT/etc/resolv.conf"
        fi
        [ -e "$STASH/ld.so.preload" ] && mv "$STASH/ld.so.preload" "$MNT/etc/ld.so.preload"
        [ -n "${QEMU_COPY:-}" ] && rm -f "$MNT$QEMU_COPY"
        for m in dev/pts dev proc sys; do
            mountpoint -q "$MNT/$m" && umount -l "$MNT/$m"
        done
        mountpoint -q "$MNT$BOOTDIR" && umount "$MNT$BOOTDIR"
        umount "$MNT"
    fi
    [ -n "$LOOP_BOOT" ] && losetup -d "$LOOP_BOOT"
    [ -n "$LOOP_ROOT" ] && losetup -d "$LOOP_ROOT"
    LOOP_BOOT=''; LOOP_ROOT=''
}
trap cleanup EXIT

# partition N start / size in bytes
part_start() { sfdisk -J "$IMG" | python3 -c "import json,sys; p=json.load(sys.stdin)['partitiontable']['partitions'][$1-1]; print(p['start']*512)"; }
part_size()  { sfdisk -J "$IMG" | python3 -c "import json,sys; p=json.load(sys.stdin)['partitiontable']['partitions'][$1-1]; print(p['size']*512)"; }
# One loop device per partition (works in containers without partition nodes).
attach() {
    LOOP_BOOT=$(losetup -f --show -o "$(part_start 1)" --sizelimit "$(part_size 1)" "$IMG")
    LOOP_ROOT=$(losetup -f --show -o "$(part_start 2)" --sizelimit "$(part_size 2)" "$IMG")
}

# ---------------------------------------------------------------------------
step "Base image"
SRC=$BASE
if [[ "$BASE" =~ ^https?:// ]]; then
    eff=$(curl -fsIL -o /dev/null -w '%{url_effective}' "$BASE")
    SRC="$CACHE/${eff##*/}"
    if [ ! -s "$SRC" ]; then
        note "downloading $eff"
        curl -fL --retry 3 -o "$SRC.part" "$eff"
        mv "$SRC.part" "$SRC"
    else
        note "cached: $SRC"
    fi
    if curl -fsL -o "$SRC.sha256" "$eff.sha256" 2>/dev/null; then
        (cd "$CACHE" && sha256sum -c --quiet "${SRC##*/}.sha256") || die "checksum mismatch for $SRC"
        note "sha256 ok"
    fi
fi
[ -f "$SRC" ] || die "no such image: $SRC"
note "base: ${SRC##*/}"
rm -f "$IMG"
case "$SRC" in
    *.xz) xz -dc -T0 "$SRC" > "$IMG" ;;
    *.zip) unzip -p "$SRC" '*.img' > "$IMG" ;;
    *.gz) gzip -dc "$SRC" > "$IMG" ;;
    *) cp --sparse=always "$SRC" "$IMG" ;;
esac

step "Grow by $GROW MB for the build"
truncate -s +"${GROW}M" "$IMG"
echo ", +" | sfdisk -q -N 2 --no-reread --no-tell-kernel "$IMG" >/dev/null
attach
e2fsck -fy "$LOOP_ROOT" >/dev/null || [ $? -le 1 ]
resize2fs "$LOOP_ROOT" >/dev/null 2>&1

step "Mount"
mkdir -p "$MNT"
mount "$LOOP_ROOT" "$MNT"
[ -d "$MNT/boot/firmware" ] || BOOTDIR=/boot
mount "$LOOP_BOOT" "$MNT$BOOTDIR"
note "root $(df -h --output=size,avail "$MNT" | tail -1 | xargs), boot at $BOOTDIR"

# Run arm64 binaries on a non-arm64 host through qemu's binfmt handler.
QEMU_COPY=
if [ -e "$MNT/lib/ld-linux-aarch64.so.1" ]; then
    if [ "$(uname -m)" != aarch64 ]; then
        BF=/proc/sys/fs/binfmt_misc/qemu-aarch64
        [ -e "$BF" ] || die "no binfmt handler for arm64: apt install qemu-user-static binfmt-support (or run on an arm64 machine)"
        interp=$(sed -n 's/^interpreter //p' "$BF")
        if ! grep -q '^flags:.*F' "$BF"; then
            QEMU_COPY=$interp
            install -D -m 755 "$interp" "$MNT$interp"
        fi
        note "arm64 through qemu ($interp): expect an hour or more"
    fi
else
    note "warning: not an arm64 image (fine for CI tests, it will not boot a CardputerZero)"
fi

step "Prepare chroot"
mkdir -p "$STASH"
for m in dev dev/pts proc sys; do
    case $m in
        proc) mount -t proc proc "$MNT/proc" ;;
        sys) mount -t sysfs sys "$MNT/sys" ;;
        *) mount --bind "/$m" "$MNT/$m" ;;
    esac
done
if [ -e "$MNT/etc/resolv.conf" ] || [ -L "$MNT/etc/resolv.conf" ]; then
    mv "$MNT/etc/resolv.conf" "$STASH/resolv.conf"
fi
cp -L /etc/resolv.conf "$MNT/etc/resolv.conf"
[ -e "$MNT/etc/ld.so.preload" ] && mv "$MNT/etc/ld.so.preload" "$STASH/ld.so.preload"
# No service may start inside the chroot.
if [ ! -e "$MNT/usr/sbin/policy-rc.d" ]; then
    printf '#!/bin/sh\nexit 101\n' > "$MNT/usr/sbin/policy-rc.d"
    chmod 755 "$MNT/usr/sbin/policy-rc.d"
    touch "$MNT/usr/sbin/policy-rc.d.zos"
fi

step "Copy ZeroOS source ($VERSION)"
[ -f "$ROOT/third_party/lvgl/lvgl.h" ] || (cd "$ROOT" && ./tools/fetch-deps.sh)
mkdir -p "$MNT/opt/zeroos/src"
rsync -a --delete --exclude=/build --exclude='/build-*' --exclude=/out --exclude=/screenshots \
    --exclude=.venv --exclude=__pycache__ --exclude=.git "$ROOT/" "$MNT/opt/zeroos/src/"
echo "$VERSION" > "$MNT/opt/zeroos/src/VERSION"

step "zeroos-setup.sh (in the image)"
ENV=(HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
     LANG=C.UTF-8 TERM=dumb ZOS_IMAGE_BUILD=1)
for v in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY \
         ZOS_M5_DTOVERLAYS_REF ZOS_M5_PIGEN_REF ZOS_OPENHOP_REF ZOS_HEADERS_PKG ZOS_KERNEL_FLAVOUR; do
    [ -n "${!v:-}" ] && ENV+=("$v=${!v}")
done
# Behind a TLS-inspecting proxy, point ZOS_BUILD_CA at its CA bundle; it is
# used for the build's downloads only and not left in the image.
if [ -n "${ZOS_BUILD_CA:-}" ]; then
    cp "$ZOS_BUILD_CA" "$MNT/tmp/zos-build-ca.pem"
    for v in SSL_CERT_FILE CURL_CA_BUNDLE GIT_SSL_CAINFO REQUESTS_CA_BUNDLE PIP_CERT; do
        ENV+=("$v=/tmp/zos-build-ca.pem")
    done
fi
start=$(date +%s)
chroot "$MNT" /usr/bin/env -i "${ENV[@]}" \
    /bin/bash /opt/zeroos/src/tools/image/zeroos-setup.sh "${SETUP_ARGS[@]}"
note "setup took $(( ($(date +%s) - start) / 60 )) min"

step "Update bundle"
# What zeroos-update installs on a running CardputerZero: the program as
# built here, plus the source tree for the helpers, units and rules.
B=$WORK/bundle
rm -rf "$B" "$MNT/tmp/zos-bundle"
mkdir -p "$B/zeroos-update"
chroot "$MNT" /usr/bin/env -i "${ENV[@]}" DESTDIR=/tmp/zos-bundle \
    cmake --install /opt/zeroos/build --strip >/dev/null
mv "$MNT/tmp/zos-bundle" "$B/zeroos-update/stage"
rsync -a --exclude=/third_party "$MNT/opt/zeroos/src/" "$B/zeroos-update/src/"
echo "$VERSION" > "$B/zeroos-update/VERSION"
BUNDLE=zeroos-update-$VERSION.tar.xz
tar -C "$B" --owner=0 --group=0 -cJf "$OUT/$BUNDLE" zeroos-update
(cd "$OUT" && sha256sum "$BUNDLE" > "$BUNDLE.sha256")
rm -rf "$B"
note "$BUNDLE ($(du -h "$OUT/$BUNDLE" | cut -f1))"

step "Tidy"
rm -rf "${MNT:?}"/tmp/* "${MNT:?}"/var/tmp/* "$MNT/root/.cache" "$MNT/opt/zeroos/build/CMakeFiles"
# A fresh machine-id on first boot (and systemd's first-boot units).
[ -e "$MNT/etc/machine-id" ] && : > "$MNT/etc/machine-id"
rm -f "$MNT/var/lib/dbus/machine-id"
cp "$MNT/etc/zeroos-release" "$OUT/$NAME.release" 2>/dev/null || true
fstrim "$MNT" >/dev/null 2>&1 || true
cleanup

step "Shrink"
attach
e2fsck -fy "$LOOP_ROOT" >/dev/null || [ $? -le 1 ]
resize2fs -M "$LOOP_ROOT" >/dev/null 2>&1
blocks=$(dumpe2fs -h "$LOOP_ROOT" 2>/dev/null | sed -n 's/^Block count: *//p')
bsize=$(dumpe2fs -h "$LOOP_ROOT" 2>/dev/null | sed -n 's/^Block size: *//p')
# Leave 256 MB so the first boot has room before the partition is expanded.
fs_bytes=$(( blocks * bsize + 256 * 1024 * 1024 ))
resize2fs "$LOOP_ROOT" "$(( fs_bytes / 1024 ))K" >/dev/null 2>&1
losetup -d "$LOOP_BOOT" "$LOOP_ROOT"; LOOP_BOOT=''; LOOP_ROOT=''
start2=$(( $(part_start 2) / 512 ))
sectors=$(( (fs_bytes + 511) / 512 ))
echo "$start2,$sectors" | sfdisk -q -N 2 --no-reread --no-tell-kernel "$IMG" >/dev/null
truncate -s $(( (start2 + sectors) * 512 + 1024 * 1024 )) "$IMG"
note "image $(( $(stat -c %s "$IMG") / 1024 / 1024 )) MB (expands to the card on first boot)"

step "Output"
if [ "$COMPRESS" = 1 ]; then
    xz -T0 -6 -c "$IMG" > "$OUT/$NAME.img.xz"
    FINAL="$OUT/$NAME.img.xz"
    [ "$KEEP" = 1 ] || rm -f "$IMG"
else
    mv "$IMG" "$OUT/$NAME.img"
    FINAL="$OUT/$NAME.img"
fi
(cd "$OUT" && sha256sum "${FINAL##*/}" > "${FINAL##*/}.sha256")
[ "$KEEP" = 1 ] || rm -rf "$MNT" "$STASH"
note "$FINAL ($(du -h "$FINAL" | cut -f1))"
note "flash it with Raspberry Pi Imager: Choose OS -> Use custom"
