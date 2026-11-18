#!/bin/bash
# Turn Raspberry Pi OS Lite (arm64, Trixie) into ZeroOS for the CardputerZero.
#
# Two ways to use it:
#   * build-image.sh runs it inside the image (chroot) to make a flashable .img
#   * on a CardputerZero already running Raspberry Pi OS Lite (or M5Stack's
#     image):  sudo ./tools/image/zeroos-setup.sh   then reboot
#
# Options (defaults in brackets):
#   --user NAME            the account ZeroOS runs as [zero]; an existing
#                          UID 1000 account (e.g. pi) is renamed to it
#   --password PW          its password [none: password login disabled;
#                          set one later in zeroos.txt or with passwd]
#   --hostname NAME        [zeroos]
#   --timezone TZ          [Europe/Brussels]
#   --country CC           Wi-Fi regulatory country [BE]
#   --region CODE          Meshtastic region [EU_868]
#   --meshcore-preset P    eu-narrow | eu-legacy | nl | us | anz [eu-narrow]
#   --board v5|v3          CardputerZero board revision [v5, the retail one]
#   --no-upgrade           skip apt full-upgrade (kernel and headers must match)
#   --no-m5-splash         keep the stock firmware (no boot splash on the panel)
#   --skip-drivers         don't build M5Stack's display/keyboard/audio drivers
#   --skip-meshtastic / --skip-meshcore / --skip-tailscale
#   --test-host            allow a non-arm64 / non-Raspberry Pi system (CI tests)
#
# Pinned upstream sources (override with the env vars):
#   ZOS_M5_DTOVERLAYS_REF  m5stack/m5stack-linux-dtoverlays commit
#   ZOS_M5_PIGEN_REF       CardputerZero/pi-gen commit (boot splash files)
#   ZOS_OPENHOP_REF        openhop-dev/openhop_repeater commit (MeshCore)
set -euo pipefail

HERE=$(cd "$(dirname "$0")/../.." && pwd)
FILES="$HERE/tools/image/files"
ZUSER=zero
ZPASS=
HOSTNAME_NEW=zeroos
TZ_NEW=Europe/Brussels
COUNTRY=BE
REGION=EU_868
MC_PRESET=eu-narrow
BOARD=v5
DO_UPGRADE=1
DO_SPLASH=1
DO_DRIVERS=1
DO_MT=1
DO_MC=1
DO_TS=1
TEST_HOST=0

M5_DTOVERLAYS_REPO=https://github.com/m5stack/m5stack-linux-dtoverlays.git
M5_DTOVERLAYS_REF=${ZOS_M5_DTOVERLAYS_REF:-75518549a2640486f5d13ea72ca4935b198d7c20}
M5_PIGEN_RAW=https://raw.githubusercontent.com/CardputerZero/pi-gen
M5_PIGEN_REF=${ZOS_M5_PIGEN_REF:-791a985d4efca7327ae1fdb6c48230ac0d1ee1bc}
OPENHOP_REPO=https://github.com/openhop-dev/openhop_repeater.git
OPENHOP_REF=${ZOS_OPENHOP_REF:-13eb8b2ea8b1cdb4a07ed6e282dc99e3aa8a5a8b}
HEADERS_PKG=${ZOS_HEADERS_PKG:-linux-headers-rpi-v8}

while [ $# -gt 0 ]; do
    case "$1" in
        --user) ZUSER=$2; shift ;;
        --password) ZPASS=$2; shift ;;
        --hostname) HOSTNAME_NEW=$2; shift ;;
        --timezone) TZ_NEW=$2; shift ;;
        --country) COUNTRY=$2; shift ;;
        --region) REGION=$2; shift ;;
        --meshcore-preset) MC_PRESET=$2; shift ;;
        --board) BOARD=$2; shift ;;
        --no-upgrade) DO_UPGRADE=0 ;;
        --no-m5-splash) DO_SPLASH=0 ;;
        --skip-drivers) DO_DRIVERS=0 ;;
        --skip-meshtastic) DO_MT=0 ;;
        --skip-meshcore) DO_MC=0 ;;
        --skip-tailscale) DO_TS=0 ;;
        --test-host) TEST_HOST=1 ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { echo "zeroos-setup: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
[[ "$ZUSER" =~ ^[a-z][-a-z0-9_]*$ ]] || die "invalid user name: $ZUSER"
case "$BOARD" in v5|v3) ;; *) die "--board must be v5 or v3" ;; esac

# In an image build there is no running system: enable units, never start them.
IMAGE_BUILD=${ZOS_IMAGE_BUILD:-0}
svc_enable() { systemctl enable "$@" >/dev/null 2>&1 || systemctl enable "$@"; }
svc_disable() { systemctl disable "$@" >/dev/null 2>&1 || true; }

. /etc/os-release
ARCH=$(dpkg --print-architecture)
if [ "$TEST_HOST" = 0 ]; then
    [ "$ARCH" = arm64 ] || die "needs arm64 Raspberry Pi OS (found $ARCH); use --test-host for CI"
fi
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
export DEBIAN_FRONTEND=noninteractive
mkdir -p /var/lib/zeroos
APT="apt-get -y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"

# ---------------------------------------------------------------------------
step "Packages"
$APT update
[ "$DO_UPGRADE" = 1 ] && $APT full-upgrade
BUILD_PKGS="build-essential cmake pkg-config git python3 device-tree-compiler \
    libmupdf-dev libfreetype-dev libharfbuzz-dev libjbig2dec0-dev libopenjp2-7-dev \
    libjpeg-dev libgumbo-dev libmujs-dev zlib1g-dev libcurl4-openssl-dev \
    libcjson-dev libvterm-dev libdrm-dev"
RUN_PKGS="fonts-dejavu-core mpv openssh-client ca-certificates curl gnupg wget \
    i2c-tools gpiod python3-venv python3-yaml polkitd sudo parted rfkill jq \
    alsa-utils network-manager exfatprogs psmisc"
# The camera app drives rpicam-vid / rpicam-still (only on Raspberry Pi OS).
CAMERA_PKGS="rpicam-apps-lite"
OPENHOP_PKGS="libffi-dev libusb-1.0-0 python3-pip python3-rrdtool swig python3-dev"
# shellcheck disable=SC2086
$APT install --no-install-recommends $BUILD_PKGS $RUN_PKGS $OPENHOP_PKGS
if [ "$TEST_HOST" = 0 ]; then
    # shellcheck disable=SC2086
    $APT install --no-install-recommends $CAMERA_PKGS || note "rpicam-apps not available: no camera app"
fi
if [ "$DO_DRIVERS" = 1 ]; then
    $APT install --no-install-recommends "$HEADERS_PKG"
fi
# The CM0 boots kernel8.img (rpi-v8); the Pi 5 kernel is dead weight here.
if dpkg -s linux-image-rpi-2712 >/dev/null 2>&1; then
    $APT purge linux-image-rpi-2712 linux-headers-rpi-2712 || true
fi
$APT autoremove --purge

# ---------------------------------------------------------------------------
step "Account '$ZUSER'"
uid1000=$(getent passwd 1000 | cut -d: -f1 || true)
if ! id "$ZUSER" >/dev/null 2>&1; then
    if [ -n "$uid1000" ]; then
        note "renaming $uid1000 -> $ZUSER"
        usermod -l "$ZUSER" -d "/home/$ZUSER" -m "$uid1000"
        getent group "$uid1000" >/dev/null && groupmod -n "$ZUSER" "$uid1000"
        rm -f "/etc/sudoers.d/010_$uid1000-nopasswd"
    else
        useradd -m -s /bin/bash "$ZUSER"
    fi
fi
for g in adm dialout cdrom sudo audio video plugdev games users input render netdev spi i2c gpio; do
    getent group "$g" >/dev/null && usermod -aG "$g" "$ZUSER"
done
echo "$ZUSER ALL=(ALL) NOPASSWD: ALL" > "/etc/sudoers.d/010_$ZUSER-nopasswd"
chmod 440 "/etc/sudoers.d/010_$ZUSER-nopasswd"
if [ -n "$ZPASS" ]; then
    echo "$ZUSER:$ZPASS" | chpasswd
    note "password set"
else
    passwd -l "$ZUSER" >/dev/null
    note "no password: set one in zeroos.txt (see README) or with passwd"
fi
ZHOME=$(getent passwd "$ZUSER" | cut -d: -f6)
# Raspberry Pi OS's first-boot "create a user" wizard has nothing to do now,
# and it would sit invisibly on a console that isn't on the panel.
svc_disable userconfig.service
systemctl mask userconfig.service >/dev/null 2>&1 || true
rm -f /etc/ssh/sshd_config.d/rename_user.conf
# M5Stack's own image: its launcher and setup wizard would fight ZeroOS for
# the panel.
if [ -d /usr/share/APPLaunch ]; then
    note "M5Stack image detected: disabling APPLaunch and LaunchWizard"
    svc_disable LaunchWizard.service
    systemctl mask LaunchWizard.service >/dev/null 2>&1 || true
    systemctl --global disable APPLaunch.service >/dev/null 2>&1 || true
    rm -f /var/lib/LaunchWizard/run-oobe /var/lib/LaunchWizard/run-keyboard-guide
fi
systemctl set-default multi-user.target >/dev/null

cat > /etc/zeroos.conf <<EOF
# Read by ZeroOS's helper scripts.
ZOS_USER=$ZUSER
ZOS_MESH_NAME=$HOSTNAME_NEW
ZOS_MESHTASTIC_REGION=$REGION
ZOS_BOARD_REV=$BOARD
EOF

# ---------------------------------------------------------------------------
step "System: hostname, time zone, Wi-Fi country, journal"
echo "$HOSTNAME_NEW" > /etc/hostname
if grep -q '^127\.0\.1\.1' /etc/hosts; then
    sed -i "s/^127\.0\.1\.1.*/127.0.1.1\t$HOSTNAME_NEW/" /etc/hosts
else
    printf '127.0.1.1\t%s\n' "$HOSTNAME_NEW" >> /etc/hosts
fi
# Raspberry Pi OS Trixie runs cloud-init on first boot (that is how Raspberry
# Pi Imager's settings get in). Keep our hostname unless the user sets one in
# zeroos.txt; Imager's Wi-Fi and user settings still work.
if [ -d /etc/cloud/cloud.cfg.d ]; then
    printf '# ZeroOS: the hostname comes from zeroos-setup / zeroos.txt\npreserve_hostname: true\n' \
        > /etc/cloud/cloud.cfg.d/99-zeroos.cfg
fi
if [ -e "/usr/share/zoneinfo/$TZ_NEW" ]; then
    ln -sf "/usr/share/zoneinfo/$TZ_NEW" /etc/localtime
    echo "$TZ_NEW" > /etc/timezone
fi
# Raspberry Pi OS keeps Wi-Fi soft-blocked until a country is set.
for f in /var/lib/systemd/rfkill/*:wlan; do [ -e "$f" ] && echo 0 > "$f"; done
mkdir -p /var/log/journal /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=persistent\nSystemMaxUse=50M\n' > /etc/systemd/journald.conf.d/zeroos.conf
echo i2c-dev > /etc/modules-load.d/cardputerzero.conf
echo "blacklist 8192cu" > /etc/modprobe.d/blacklist-8192cu.conf
# The panel backlight is a PWM the driver owns; systemd must not restore it to 0.
ln -sf /dev/null "/etc/systemd/system/systemd-backlight@backlight:backlight.service"

# ---------------------------------------------------------------------------
step "Boot configuration (config.txt, cmdline.txt)"
CFG="$BOOT/config.txt"
CMD="$BOOT/cmdline.txt"
if [ -f "$CFG" ]; then
    # Drop settings we manage wherever they are, then append one [all] block.
    # This is the configuration M5Stack's own image uses for the CardputerZero.
    sed -i -e '/^# --- ZeroOS/,/^# --- end ZeroOS/d' \
        -e '/^camera_auto_detect=/d' -e '/^dtparam=\(i2c_arm\|i2s\|spi\|audio\)=/d' \
        -e '/^enable_uart=/d' -e '/^dtoverlay=imx219$/d' \
        -e '/^dtoverlay=\(cardputerzero\|bq27220\|bmi270\|spk-gpio24\|camera-gpio16\|gpio-ir\)/d' \
        -e '/^splash_screen=/d' -e '/^gpu_mem=/d' "$CFG"
    # CMA holds the HDMI and video buffers: 1080p output, mpv's frames and the
    # H.264 decoder. It stays usable for ordinary pages while unused.
    sed -i -e 's/^dtoverlay=vc4-kms-v3d.*$/dtoverlay=vc4-kms-v3d,cma-96/' "$CFG"
    if [ "$BOARD" = v5 ]; then
        BOARD_OVERLAYS="dtoverlay=cardputerzero-v5-overlay
dtoverlay=bq27220_v5
dtoverlay=bmi270_bmm150_overlay"
    else
        BOARD_OVERLAYS="dtoverlay=cardputerzero-v3-overlay
dtoverlay=bq27220_v3
dtoverlay=spk-gpio24-high-overlay
dtoverlay=bmi270_overlay
dtoverlay=camera-gpio16-high-overlay"
    fi
    cat >> "$CFG" <<EOF

# --- ZeroOS / CardputerZero (written by zeroos-setup.sh) ---
[all]
camera_auto_detect=0
dtoverlay=imx219
dtparam=i2c_arm=on
dtparam=i2s=on
dtparam=spi=on
dtparam=audio=off
# UART on GPIO14/15: the LoRa cap's GPS
enable_uart=1
dtoverlay=dwc2
$BOARD_OVERLAYS
dtoverlay=gpio-ir,gpio_pin=13,gpio_pull=up
dtoverlay=gpio-ir-tx,gpio_pin=12
# --- end ZeroOS ---
EOF
    if [ "$DO_SPLASH" = 1 ]; then
        sed -i '1i splash_screen=CardputerZero_splash_custom.bin\ngpu_mem=128' "$CFG"
    fi
else
    note "no $CFG (not a Raspberry Pi image?)"
fi
if [ -f "$CMD" ]; then
    # No login console on the GPS UART or the panel; ZeroOS owns the screen.
    awk -v cc="$COUNTRY" '{
        out = ""
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^console=(serial0|ttyAMA0|ttyS0)/ || $i == "splash" || $i == "resize" ||
                $i == "plymouth.ignore-serial-consoles" || $i == "quiet" ||
                $i ~ /^fbcon=/ || $i ~ /^cfg80211\.ieee80211_regdom=/) continue
            out = out (out == "" ? "" : " ") $i
        }
        print out " quiet fbcon=map:off cfg80211.ieee80211_regdom=" cc
    }' "$CMD" > "$CMD.tmp" && mv "$CMD.tmp" "$CMD"
fi
if [ "$DO_SPLASH" = 1 ] && [ -d "$BOOT" ]; then
    # M5Stack's boot firmware build shows the CardputerZero splash on the
    # panel while Linux starts. Pinned and checksummed.
    while read -r sum name; do
        curl -fsSL --retry 3 -o "$BOOT/$name.zos" \
            "$M5_PIGEN_RAW/$M5_PIGEN_REF/stage2/05-cardputerzero/files/$name"
        echo "$sum  $BOOT/$name.zos" | sha256sum -c --quiet - || die "checksum mismatch: $name"
        mv "$BOOT/$name.zos" "$BOOT/$name"
    done <<'EOF'
f5baaa1f16918a4f2f8a83c5e762d6134bb8f9348855ba6ef56498ccf65378dc start.elf
c70883de67d4e02919830c5084fcfe2de058ec80748cb57d5dd9bebd48fb4be5 fixup.dat
21466ad04871484b878dc18b409a61ba97e5e08880a0e48f35f3f7420a270091 splash.bmp
a90f7ee2e71802442de66ca6f99d77ad3b8b9d9ace1e633d2cc709b90181e971 CardputerZero_splash_custom.bin
EOF
    note "boot splash installed"
fi

# ---------------------------------------------------------------------------
if [ "$DO_DRIVERS" = 1 ]; then
    step "CardputerZero drivers (M5Stack m5stack-linux-dtoverlays)"
    SRC=/opt/zeroos/m5-dtoverlays
    if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD)" != "$M5_DTOVERLAYS_REF" ]; then
        rm -rf "$SRC"
        git clone -q --filter=blob:none "$M5_DTOVERLAYS_REPO" "$SRC"
        git -C "$SRC" -c advice.detachedHead=false checkout -q "$M5_DTOVERLAYS_REF"
    fi
    install -m 755 "$FILES/zeroos-m5-drivers" /usr/local/sbin/zeroos-m5-drivers
    # Rebuild automatically whenever apt installs a new kernel or its headers.
    for d in /etc/kernel/postinst.d /etc/kernel/header_postinst.d; do
        install -d "$d"
        ln -sf /usr/local/sbin/zeroos-m5-drivers "$d/zz-zeroos-m5-drivers"
    done
    mkdir -p "$BOOT/overlays"
    ZOS_BOARD_REV=$BOARD /usr/local/sbin/zeroos-m5-drivers
    # M5Stack's drivers track Raspberry Pi's kernel; a new kernel series can
    # break them. Keep the kernel the drivers were built for until you choose
    # to move (sudo apt-mark unhold ...; the hook above then rebuilds).
    if [ "$TEST_HOST" = 0 ]; then
        apt-mark hold linux-image-rpi-v8 "$HEADERS_PKG" >/dev/null
        note "kernel packages held at $(dpkg-query -W -f='${Version}' linux-image-rpi-v8 2>/dev/null)"
    fi
fi

# ---------------------------------------------------------------------------
step "ZeroOS"
if [ ! -f "$HERE/third_party/lvgl/lvgl.h" ]; then
    (cd "$HERE" && ./tools/fetch-deps.sh)
fi
cmake -S "$HERE" -B /opt/zeroos/build -DCMAKE_BUILD_TYPE=Release -DZOS_WITH_SDL=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
cmake --build /opt/zeroos/build -j"$(nproc)"
cmake --install /opt/zeroos/build >/dev/null
install -m 755 "$FILES/zeroos-session" /usr/local/bin/zeroos-session
install -d -o "$ZUSER" -g "$ZUSER" "$ZHOME/Books" "$ZHOME/Music" "$ZHOME/Videos" "$ZHOME/Pictures"
cp -n "$HERE"/assets/books/*.epub "$ZHOME/Books/" 2>/dev/null || true
chown -R "$ZUSER:$ZUSER" "$ZHOME/Books"
cat > /etc/systemd/system/zeroos.service <<EOF
# ZeroOS shell on the CardputerZero panel (written by zeroos-setup.sh)
[Unit]
Description=ZeroOS shell
After=systemd-user-sessions.service zeroos-firstboot.service fb_load.service data.mount
# Stop the login console from drawing over the framebuffer.
Conflicts=getty@tty1.service
# M5Stack's boot splash (fb_load) holds the panel as DRM master until
# M5Stack's own launcher runs; while it does, nothing drawn through the
# framebuffer appears. Stop it first.
Conflicts=fb_load.service

[Service]
User=$ZUSER
Environment=ZOS_BOOKS=$ZHOME/Books
ExecStartPre=+/bin/sh -c 'echo 0 > /sys/class/graphics/fbcon/cursor_blink || true'
ExecStart=/usr/local/bin/zeroos-session
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
install -d /etc/polkit-1/rules.d
sed "s/@ZOS_USER@/$ZUSER/g" "$HERE/system/50-zeroos-system.rules" > /etc/polkit-1/rules.d/50-zeroos-system.rules
install -m 644 "$HERE/system/70-zeroos-backlight.rules" /etc/udev/rules.d/70-zeroos-backlight.rules
install -m 755 "$FILES/zeroos-usb" /usr/local/sbin/zeroos-usb
# The first boot splits the card into the system and the USB drive area
# (zeroos-usb firstboot-layout), so Raspberry Pi OS must not grow the root
# file system over the whole card first.
systemctl mask rpi-resize.service >/dev/null 2>&1 || true
install -m 755 "$FILES/zeroos-firstboot" /usr/local/sbin/zeroos-firstboot
install -m 644 "$FILES/zeroos-firstboot.service" /etc/systemd/system/
install -m 755 "$FILES/zeroos-bootlog" /usr/local/sbin/zeroos-bootlog
install -m 644 "$FILES/zeroos-bootlog.service" "$FILES/zeroos-bootlog-snapshot.service" \
    "$FILES/zeroos-bootlog-snapshot.timer" /etc/systemd/system/
svc_enable zeroos.service zeroos-firstboot.service zeroos-bootlog.service zeroos-bootlog-snapshot.timer
VERSION=$(git -C "$HERE" describe --always --dirty 2>/dev/null || cat "$HERE/VERSION" 2>/dev/null || echo unknown)
printf 'ZEROOS_VERSION=%s\nZEROOS_BUILD_DATE=%s\nBASE=%s\n' "$VERSION" "$(date -u +%F)" \
    "${PRETTY_NAME:-unknown}" > /etc/zeroos-release
note "zeroos $VERSION -> /usr/local/bin/zeroos (service: zeroos.service, user $ZUSER)"

# ---------------------------------------------------------------------------
SYS="$HERE/system/mesh"
if [ "$DO_MT" = 1 ] || [ "$DO_MC" = 1 ]; then
    step "LoRa cap: init script, daemon exclusivity, polkit"
    install -m 755 "$SYS/zos-lora-cap-init" /usr/local/sbin/zos-lora-cap-init
    for unit in meshtasticd openhop-repeater; do
        install -d "/etc/systemd/system/$unit.service.d"
        install -m 644 "$SYS/$unit.service.d/zeroos.conf" "/etc/systemd/system/$unit.service.d/zeroos.conf"
    done
    install -d /etc/polkit-1/rules.d
    sed "s/@ZOS_USER@/$ZUSER/" "$SYS/50-zeroos-radio.rules" > /etc/polkit-1/rules.d/50-zeroos-radio.rules
fi

if [ "$DO_MT" = 1 ]; then
    step "Meshtastic: meshtasticd"
    if ! dpkg -s meshtasticd >/dev/null 2>&1; then
        case "$ARCH" in
            arm64|amd64) DIST="Debian_${VERSION_ID}" ;;
            armhf)       DIST="Raspbian_${VERSION_ID}" ;;
            *) die "unsupported architecture $ARCH" ;;
        esac
        echo "deb http://download.opensuse.org/repositories/network:/Meshtastic:/beta/$DIST/ /" \
            > /etc/apt/sources.list.d/network:Meshtastic:beta.list
        curl -fsSL "https://download.opensuse.org/repositories/network:Meshtastic:beta/$DIST/Release.key" \
            | gpg --dearmor > /etc/apt/trusted.gpg.d/network_Meshtastic_beta.gpg
        $APT update
        $APT install meshtasticd
    fi
    install -d /etc/meshtasticd/config.d
    install -m 644 "$SYS/lora-cardputerzero-cap1262.yaml" /etc/meshtasticd/config.d/
    svc_disable meshtasticd.service          # ZeroOS starts whichever radio app you open
    CLI=/opt/zeroos/meshtastic-cli
    [ -x "$CLI/bin/meshtastic" ] || { python3 -m venv "$CLI" && "$CLI/bin/pip" install -q meshtastic; }
    install -m 755 "$FILES/zos-meshtastic-setup" /usr/local/sbin/zos-meshtastic-setup
    install -m 644 "$FILES/zos-meshtastic-setup.service" /etc/systemd/system/
    svc_enable zos-meshtastic-setup.service
    note "region $REGION and owner '$HOSTNAME_NEW' are set the first time meshtasticd runs"
fi

if [ "$DO_MC" = 1 ]; then
    step "MeshCore: openHop (pyMC) repeater"
    # The steps of openHop's interactive 'manage.sh install', without the TUI
    # or the SPI check (there is no SPI in an image build).
    OH_SRC=/opt/zeroos/openhop-src
    OH_VENV=/opt/openhop_repeater/venv
    if [ ! -d "$OH_SRC/.git" ] || [ "$(git -C "$OH_SRC" rev-parse HEAD)" != "$OPENHOP_REF" ]; then
        rm -rf "$OH_SRC"
        git clone -q --filter=blob:none "$OPENHOP_REPO" "$OH_SRC"
        git -C "$OH_SRC" -c advice.detachedHead=false checkout -q "$OPENHOP_REF"
    fi
    id repeater >/dev/null 2>&1 || useradd --system --home /var/lib/openhop_repeater --shell /sbin/nologin repeater
    for g in plugdev dialout gpio i2c spi; do
        getent group "$g" >/dev/null && usermod -aG "$g" repeater
    done
    install -d /opt/openhop_repeater /etc/openhop_repeater /var/log/openhop_repeater \
        /var/lib/openhop_repeater/.config/openhop_repeater
    [ -x "$OH_VENV/bin/python" ] || python3 -m venv --system-site-packages "$OH_VENV"
    "$OH_VENV/bin/python" -m pip install -q --upgrade pip setuptools wheel setuptools_scm
    OH_VERSION=$(cd "$OH_SRC" && "$OH_VENV/bin/python" -m setuptools_scm 2>/dev/null || echo 0.0.0)
    SETUPTOOLS_SCM_PRETEND_VERSION_FOR_OPENHOP_REPEATER=$OH_VERSION \
    PIP_ONLY_BINARY=pycryptodome,cffi,PyNaCl,psutil \
        "$OH_VENV/bin/python" -m pip install -q "${OH_SRC}[hardware]"
    cp "$OH_SRC/radio-settings.json" "$OH_SRC/radio-presets.json" /var/lib/openhop_repeater/ 2>/dev/null || true
    cp "$OH_SRC/config.yaml.example" /etc/openhop_repeater/config.yaml.example
    [ -f /etc/openhop_repeater/config.yaml ] || cp "$OH_SRC/config.yaml.example" /etc/openhop_repeater/config.yaml
    install -m 644 "$OH_SRC/openhop-repeater.service" /etc/systemd/system/openhop-repeater.service
    cat > /etc/polkit-1/rules.d/10-openhop-repeater.rules <<'EOF'
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.systemd1.manage-units" &&
        action.lookup("unit") == "openhop-repeater.service" &&
        subject.user == "repeater") {
        return polkit.Result.YES;
    }
});
EOF
    "$OH_VENV/bin/python" "$SYS/configure_openhop.py" --name "$HOSTNAME_NEW" --preset "$MC_PRESET"
    chown -R repeater:repeater /etc/openhop_repeater /var/log/openhop_repeater /var/lib/openhop_repeater
    chmod 750 /etc/openhop_repeater /var/log/openhop_repeater
    svc_disable openhop-repeater.service
    note "openHop $OH_VERSION, preset $MC_PRESET, companion for ZeroOS on 127.0.0.1:5000"
fi

# ---------------------------------------------------------------------------
if [ "$DO_TS" = 1 ]; then
    step "Tailscale"
    if ! dpkg -s tailscale >/dev/null 2>&1; then
        case "$ID" in debian|raspbian|ubuntu) ;; *) die "Tailscale: unsupported OS $ID" ;; esac
        curl -fsSL "https://pkgs.tailscale.com/stable/$ID/$VERSION_CODENAME.noarmor.gpg" \
            > /usr/share/keyrings/tailscale-archive-keyring.gpg
        curl -fsSL "https://pkgs.tailscale.com/stable/$ID/$VERSION_CODENAME.tailscale-keyring.list" \
            > /etc/apt/sources.list.d/tailscale.list
        $APT update
        $APT install tailscale
    fi
    svc_enable tailscaled.service
    if [ "$IMAGE_BUILD" = 0 ] && systemctl is-active -q tailscaled; then
        tailscale set --operator="$ZUSER" && touch /var/lib/zeroos/tailscale-operator 2>/dev/null || true
    fi
    note "the ZeroOS user becomes operator on first boot; sign in from the Tailscale app"
fi

# ---------------------------------------------------------------------------
step "Clean up"
$APT clean
rm -rf /var/lib/apt/lists/* /root/.cache/pip
note "done: ZeroOS $VERSION for user $ZUSER"
[ "$IMAGE_BUILD" = 0 ] && note "reboot to start ZeroOS"
exit 0
