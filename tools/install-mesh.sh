#!/bin/bash
# Set up the Cap LoRa-1262 for ZeroOS's Meshtastic and MeshCore apps.
#
#   sudo ./tools/install-mesh.sh [options]
#
#   --user NAME          user ZeroOS runs as (default: the sudo user, else pi)
#   --name NAME          node name on both meshes (default: hostname)
#   --region CODE        Meshtastic region (default: EU_868)
#   --meshcore-preset P  eu-narrow | eu-legacy | nl | us | anz (default: eu-narrow)
#   --skip-meshtastic / --skip-meshcore
#
# Safe to run again. The first run enables SPI/I2C/UART and asks for a
# reboot; run it once more afterwards to install MeshCore and set the
# Meshtastic region and owner (both need SPI up).
#
# What it installs:
#   meshtasticd               official package (download.opensuse.org repo)
#   openHop (pyMC) repeater   MeshCore stack, in monitor mode, with a
#                             companion for ZeroOS on 127.0.0.1:5000
#   zos-lora-cap-init         antenna-switch init, run before either daemon
#   systemd drop-ins          the two daemons Conflict= with each other
#   polkit rule               the ZeroOS user may start/stop just those two
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SYS="$HERE/system/mesh"
ZUSER=${SUDO_USER:-pi}
NAME=$(hostname)
REGION=EU_868
MC_PRESET=eu-narrow
DO_MT=1
DO_MC=1
OPENHOP_REPO=https://github.com/pyMC-dev/pyMC_Repeater.git
SRC=/opt/zeroos/src

while [ $# -gt 0 ]; do
    case "$1" in
        --user) ZUSER=$2; shift ;;
        --name) NAME=$2; shift ;;
        --region) REGION=$2; shift ;;
        --meshcore-preset) MC_PRESET=$2; shift ;;
        --skip-meshtastic) DO_MT=0 ;;
        --skip-meshcore) DO_MC=0 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
id "$ZUSER" >/dev/null 2>&1 || { echo "no such user: $ZUSER (use --user)" >&2; exit 1; }

# --------------------------------------------------------------------------
step "Interfaces: SPI, I2C, UART"
NEED_REBOOT=0
if command -v raspi-config >/dev/null; then
    [ -e /dev/spidev0.1 ] || { raspi-config nonint do_spi 0; NEED_REBOOT=1; }
    [ -e /dev/i2c-1 ] || { raspi-config nonint do_i2c 0; NEED_REBOOT=1; }
    # UART on GPIO14/15 for the cap's GPS, without a login console on it
    if ! grep -q '^enable_uart=1' /boot/firmware/config.txt /boot/config.txt 2>/dev/null; then
        raspi-config nonint do_serial_hw 0
        raspi-config nonint do_serial_cons 1
        NEED_REBOOT=1
    fi
else
    note "raspi-config not found: make sure config.txt has"
    note "  dtparam=spi=on, dtparam=i2c_arm=on, enable_uart=1"
    note "and that cmdline.txt has no console=serial0"
fi
usermod -aG spi,i2c,gpio,dialout "$ZUSER" 2>/dev/null || true

step "Packages"
apt-get update -qq
apt-get install -y -qq i2c-tools gpiod curl gnupg git python3-venv ca-certificates >/dev/null

step "Cap init, systemd drop-ins, polkit rule"
install -m 755 "$SYS/zos-lora-cap-init" /usr/local/sbin/zos-lora-cap-init
for unit in meshtasticd openhop-repeater; do
    install -d "/etc/systemd/system/$unit.service.d"
    install -m 644 "$SYS/$unit.service.d/zeroos.conf" "/etc/systemd/system/$unit.service.d/zeroos.conf"
done
install -d /etc/polkit-1/rules.d
sed "s/@ZOS_USER@/$ZUSER/" "$SYS/50-zeroos-radio.rules" > /etc/polkit-1/rules.d/50-zeroos-radio.rules
systemctl daemon-reload
note "radio daemons: start/stop allowed for '$ZUSER'"

# --------------------------------------------------------------------------
if [ $DO_MT = 1 ]; then
    step "Meshtastic: meshtasticd"
    if ! command -v meshtasticd >/dev/null; then
        . /etc/os-release
        case "$(dpkg --print-architecture)" in
            arm64|amd64) DIST="Debian_${VERSION_ID}" ;;
            armhf)       DIST="Raspbian_${VERSION_ID}" ;;
            *) echo "unsupported architecture $(dpkg --print-architecture)" >&2; exit 1 ;;
        esac
        REPO="http://download.opensuse.org/repositories/network:/Meshtastic:/beta/$DIST/"
        echo "deb $REPO /" > /etc/apt/sources.list.d/network:Meshtastic:beta.list
        curl -fsSL "https://download.opensuse.org/repositories/network:Meshtastic:beta/$DIST/Release.key" \
            | gpg --dearmor > /etc/apt/trusted.gpg.d/network_Meshtastic_beta.gpg
        apt-get update -qq
        apt-get install -y meshtasticd
    fi
    install -d /etc/meshtasticd/config.d
    install -m 644 "$SYS/lora-cardputerzero-cap1262.yaml" /etc/meshtasticd/config.d/
    # ZeroOS decides which daemon runs; neither starts on its own at boot.
    systemctl disable meshtasticd >/dev/null 2>&1 || true
    note "config: /etc/meshtasticd/config.d/lora-cardputerzero-cap1262.yaml"
fi

# --------------------------------------------------------------------------
if [ $NEED_REBOOT = 1 ]; then
    step "Reboot needed"
    note "SPI/I2C/UART were just enabled. Reboot, then run this script again to"
    note "install MeshCore (its installer needs SPI) and set the Meshtastic region."
    exit 0
fi

# --------------------------------------------------------------------------
if [ $DO_MC = 1 ]; then
    step "MeshCore: openHop (pyMC) repeater"
    if [ ! -f /etc/openhop_repeater/config.yaml ]; then
        install -d "$SRC"
        [ -d "$SRC/pyMC_Repeater" ] || git clone --depth 1 "$OPENHOP_REPO" "$SRC/pyMC_Repeater"
        (cd "$SRC/pyMC_Repeater" && bash ./manage.sh install)
    fi
    systemctl stop openhop-repeater >/dev/null 2>&1 || true
    /opt/openhop_repeater/venv/bin/python "$SYS/configure_openhop.py" \
        --name "$NAME" --preset "$MC_PRESET"
    systemctl disable openhop-repeater >/dev/null 2>&1 || true
    note "web dashboard: http://$(hostname).local:8000 (while MeshCore owns the radio)"
fi

if [ $DO_MT = 1 ]; then
    step "Meshtastic: region $REGION, owner '$NAME'"
    CLI=/opt/zeroos/meshtastic-cli
    [ -x "$CLI/bin/meshtastic" ] || { python3 -m venv "$CLI" && "$CLI/bin/pip" install -q meshtastic; }
    systemctl start meshtasticd
    for _ in $(seq 1 30); do
        (exec 3<>/dev/tcp/127.0.0.1/4403) 2>/dev/null && break
        sleep 1
    done
    short=$(echo "$NAME" | tr -cd 'A-Za-z0-9' | cut -c1-4 | tr a-z A-Z)
    if "$CLI/bin/meshtastic" --host 127.0.0.1 --set lora.region "$REGION" \
            --set-owner "$NAME" --set-owner-short "${short:-ZERO}"; then
        note "done"
    else
        note "could not reach meshtasticd: check 'journalctl -u meshtasticd'"
        note "(radio not found usually means the cap or SPI isn't up)"
    fi
    systemctl stop meshtasticd || true
fi

step "Ready"
note "Open Meshtastic or MeshCore in ZeroOS; it starts the right daemon."
note "Only one can use the cap at a time; the apps offer to switch."
