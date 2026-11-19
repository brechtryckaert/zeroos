# Debian packages ZeroOS needs. Sourced by zeroos-setup.sh (a new install)
# and zeroos-install-files.sh --deps (an update adds whatever is missing).
# shellcheck shell=bash disable=SC2034
BUILD_PKGS="build-essential cmake pkg-config git python3 device-tree-compiler \
    libmupdf-dev libfreetype-dev libharfbuzz-dev libjbig2dec0-dev libopenjp2-7-dev \
    libjpeg-dev libgumbo-dev libmujs-dev zlib1g-dev libcurl4-openssl-dev \
    libcjson-dev libvterm-dev libdrm-dev"
RUN_PKGS="fonts-dejavu-core mpv openssh-client ca-certificates curl gnupg wget \
    i2c-tools gpiod python3-venv python3-yaml polkitd sudo parted rfkill jq \
    alsa-utils network-manager exfatprogs psmisc xz-utils"
# The camera app drives rpicam-vid / rpicam-still (only on Raspberry Pi OS).
CAMERA_PKGS="rpicam-apps-lite"
OPENHOP_PKGS="libffi-dev libusb-1.0-0 python3-pip python3-rrdtool swig python3-dev"
