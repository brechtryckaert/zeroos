#!/bin/bash
# Set up what ZeroOS's Tailscale, SSH and Emby apps need on the device.
#
#   sudo ./tools/install-net.sh [--user NAME] [--skip-tailscale]
#
#   Tailscale  official package (tailscale.com/install.sh), tailscaled enabled
#              at boot, and NAME made "operator" so ZeroOS can connect,
#              disconnect, sign in and pick exit nodes without sudo.
#              Sign in from the Tailscale app: it shows a QR code.
#   SSH        openssh-client (the app runs the real ssh in a terminal)
#   Emby       mpv for playback; NAME joins video/render so mpv can draw
#              straight to the panel
#
# Safe to run again.
set -euo pipefail

ZUSER=${SUDO_USER:-pi}
DO_TS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --user) ZUSER=$2; shift ;;
        --skip-tailscale) DO_TS=0 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
id "$ZUSER" >/dev/null 2>&1 || { echo "no such user: $ZUSER (use --user)" >&2; exit 1; }

step "Packages: mpv, OpenSSH client, runtime libraries"
apt-get update -qq
apt-get install -y -qq mpv openssh-client libvterm0 libcjson1 libcurl4 ca-certificates curl >/dev/null
usermod -aG video,render "$ZUSER" 2>/dev/null || true
note "mpv $(mpv --version | head -1 | cut -d' ' -f2)"

if [ $DO_TS = 1 ]; then
    step "Tailscale"
    if ! command -v tailscale >/dev/null; then
        curl -fsSL https://tailscale.com/install.sh | sh
    fi
    systemctl enable --now tailscaled
    for _ in $(seq 1 20); do [ -S /var/run/tailscale/tailscaled.sock ] && break; sleep 0.5; done
    tailscale set --operator="$ZUSER"
    note "operator: $ZUSER (ZeroOS can manage the connection without sudo)"
    note "sign in from the Tailscale app in ZeroOS (scan the QR code)"
fi

step "Ready"
note "Log out and back in (or restart ZeroOS) for the new group memberships."
