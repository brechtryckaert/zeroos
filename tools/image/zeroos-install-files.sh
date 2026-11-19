#!/bin/bash
# Put ZeroOS's own files in place: the program, its helper scripts, systemd
# units, polkit and udev rules, and /etc/zeroos-release.
#
# Used in two places, so a new install and an update end up identical:
#   * zeroos-setup.sh, after building ZeroOS (cmake --install has already
#     put the program in /usr/local)
#   * zeroos-update, from a prebuilt update bundle (--stage)
#
#   zeroos-install-files.sh --src DIR [--stage DIR] [--user NAME] [--version V]
#   zeroos-install-files.sh --deps     install packages this version needs
#                                      that the system doesn't have yet
#
#   --src DIR      the ZeroOS source tree (for tools/image/files and system/)
#   --stage DIR    a `cmake --install` DESTDIR tree (usr/local/...) to copy in
#   --user NAME    the account ZeroOS runs as [ZOS_USER in /etc/zeroos.conf]
#   --version V    written to /etc/zeroos-release
#
# ZOS_ROOT=/some/dir installs into another root instead of / (tests); then
# nothing is enabled or reloaded.
set -euo pipefail

R=${ZOS_ROOT:-}
SRC=
STAGE=
ZUSER=
VERSION=
DEPS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --src) SRC=$2; shift ;;
        --stage) STAGE=$2; shift ;;
        --user) ZUSER=$2; shift ;;
        --version) VERSION=$2; shift ;;
        --deps) DEPS=1 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "zeroos-install-files: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
die() { echo "zeroos-install-files: $*" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")/../.." && pwd)

if [ "$DEPS" = 1 ]; then
    [ -n "$R" ] && exit 0
    . "$HERE/tools/image/packages.sh"
    missing=
    for p in $RUN_PKGS; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'ok installed' || missing="$missing $p"
    done
    [ -z "$missing" ] && exit 0
    echo "installing:$missing"
    export DEBIAN_FRONTEND=noninteractive
    APT="apt-get -y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"
    $APT update >/dev/null
    # shellcheck disable=SC2086
    $APT install --no-install-recommends $missing
    exit 0
fi

[ -n "$SRC" ] || SRC=$HERE
FILES="$SRC/tools/image/files"
SYS="$SRC/system"
[ -d "$FILES" ] || die "no $FILES"
if [ -z "$ZUSER" ] && [ -f "$R/etc/zeroos.conf" ]; then
    ZUSER=$(sed -n 's/^ZOS_USER=//p' "$R/etc/zeroos.conf")
fi
[ -n "$ZUSER" ] || die "no --user and no ZOS_USER in /etc/zeroos.conf"
if [ -n "$R" ]; then
    ZHOME=$(sed -n "s/^$ZUSER:[^:]*:[^:]*:[^:]*:[^:]*:\([^:]*\):.*/\1/p" "$R/etc/passwd" 2>/dev/null)
else
    ZHOME=$(getent passwd "$ZUSER" | cut -d: -f6)
fi
[ -n "$ZHOME" ] || ZHOME=/home/$ZUSER

# Files are replaced, never written into: the running ZeroOS keeps its
# old (deleted) binary until it restarts.
put() {   # put MODE SOURCE DEST
    install -D -m "$1" "$2" "$R$3.zos-new"
    mv -f "$R$3.zos-new" "$R$3"
}

if [ -n "$STAGE" ]; then
    [ -x "$STAGE/usr/local/bin/zeroos" ] || die "no usr/local/bin/zeroos in $STAGE"
    (cd "$STAGE" && find . -type f) | while read -r f; do
        f=${f#.}
        put "$(stat -c %a "$STAGE$f")" "$STAGE$f" "$f"
    done
fi

put 755 "$FILES/zeroos-session" /usr/local/bin/zeroos-session
put 755 "$FILES/zeroos-release-panel" /usr/local/sbin/zeroos-release-panel
put 755 "$FILES/zeroos-usb" /usr/local/sbin/zeroos-usb
put 755 "$FILES/zeroos-update" /usr/local/sbin/zeroos-update
put 755 "$FILES/zeroos-firstboot" /usr/local/sbin/zeroos-firstboot
put 755 "$FILES/zeroos-bootlog" /usr/local/sbin/zeroos-bootlog
for u in zeroos-firstboot.service zeroos-bootlog.service zeroos-bootlog-snapshot.service \
         zeroos-bootlog-snapshot.timer; do
    put 644 "$FILES/$u" "/etc/systemd/system/$u"
done

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
cat > "$TMP" <<EOF
# ZeroOS shell on the CardputerZero panel (written by zeroos-install-files.sh)
[Unit]
Description=ZeroOS shell
After=systemd-user-sessions.service zeroos-firstboot.service fb_load.service data.mount
# Stop the login console from drawing over the framebuffer.
Conflicts=getty@tty1.service
# Never give up restarting (after an update, boot-check counts the tries).
StartLimitIntervalSec=0

[Service]
User=$ZUSER
Environment=ZOS_BOOKS=$ZHOME/Books
# M5Stack's boot splash (fb_load) holds the panel until M5Stack's own
# launcher runs; stop it first, as root (+), or ZeroOS draws unseen.
ExecStartPre=+/usr/local/sbin/zeroos-release-panel
# After an update: go back to the previous version if this one keeps failing.
ExecStartPre=+-/usr/local/sbin/zeroos-update boot-check
ExecStartPre=+/bin/sh -c 'echo 0 > /sys/class/graphics/fbcon/cursor_blink || true'
ExecStart=/usr/local/bin/zeroos-session
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
put 644 "$TMP" /etc/systemd/system/zeroos.service
sed "s/@ZOS_USER@/$ZUSER/g" "$SYS/50-zeroos-system.rules" > "$TMP"
put 644 "$TMP" /etc/polkit-1/rules.d/50-zeroos-system.rules
put 644 "$SYS/70-zeroos-backlight.rules" /etc/udev/rules.d/70-zeroos-backlight.rules

# The LoRa cap's files, where the radios are installed.
if [ -d "$R/etc/systemd/system/meshtasticd.service.d" ] || [ -d "$R/etc/systemd/system/openhop-repeater.service.d" ]; then
    put 755 "$SYS/mesh/zos-lora-cap-init" /usr/local/sbin/zos-lora-cap-init
    for unit in meshtasticd openhop-repeater; do
        [ -d "$R/etc/systemd/system/$unit.service.d" ] &&
            put 644 "$SYS/mesh/$unit.service.d/zeroos.conf" "/etc/systemd/system/$unit.service.d/zeroos.conf"
    done
    sed "s/@ZOS_USER@/$ZUSER/" "$SYS/mesh/50-zeroos-radio.rules" > "$TMP"
    put 644 "$TMP" /etc/polkit-1/rules.d/50-zeroos-radio.rules
fi
if [ -d "$R/etc/meshtasticd/config.d" ]; then
    put 644 "$SYS/mesh/lora-cardputerzero-cap1262.yaml" /etc/meshtasticd/config.d/lora-cardputerzero-cap1262.yaml
fi
[ -e "$R/usr/local/sbin/zos-meshtastic-setup" ] && put 755 "$FILES/zos-meshtastic-setup" /usr/local/sbin/zos-meshtastic-setup
[ -e "$R/etc/systemd/system/zos-meshtastic-setup.service" ] &&
    put 644 "$FILES/zos-meshtastic-setup.service" /etc/systemd/system/zos-meshtastic-setup.service

if [ -z "$VERSION" ]; then
    VERSION=$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || cat "$SRC/VERSION" 2>/dev/null || echo unknown)
fi
BASE=$( (. "$R/etc/os-release" 2>/dev/null && echo "${PRETTY_NAME:-unknown}") || echo unknown)
printf 'ZEROOS_VERSION=%s\nZEROOS_BUILD_DATE=%s\nBASE=%s\n' "$VERSION" "$(date -u +%F)" "$BASE" > "$TMP"
put 644 "$TMP" /etc/zeroos-release

if [ -z "$R" ]; then
    systemctl enable zeroos.service zeroos-firstboot.service zeroos-bootlog.service \
        zeroos-bootlog-snapshot.timer >/dev/null 2>&1 || true
    if [ "${ZOS_IMAGE_BUILD:-0}" = 0 ] && [ -d /run/systemd/system ]; then
        systemctl daemon-reload || true
        udevadm control --reload 2>/dev/null || true
    fi
fi
echo "zeroos $VERSION installed for user $ZUSER"
