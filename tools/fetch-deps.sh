#!/bin/sh
# Fetch build dependencies. Run from the repo root.
#   Debian / Ubuntu / Raspberry Pi OS:  sudo ./tools/fetch-deps.sh --apt
set -e

if [ "$1" = "--apt" ]; then
    apt-get update
    apt-get install -y build-essential cmake pkg-config git python3 \
        libmupdf-dev libfreetype-dev libharfbuzz-dev libjbig2dec0-dev \
        libopenjp2-7-dev libjpeg-dev libgumbo-dev libmujs-dev zlib1g-dev \
        libcurl4-openssl-dev libcjson-dev libvterm-dev \
        fonts-dejavu-core mpv openssh-client
    # The SDL simulator is only needed on a desktop
    [ "$(uname -m)" = "x86_64" ] && apt-get install -y libsdl2-dev || true
fi

if [ ! -f third_party/lvgl/lvgl.h ]; then
    if [ -d .git ]; then
        git submodule update --init --depth 1 third_party/lvgl
    else
        git clone --depth 1 --branch release/v9.3 https://github.com/lvgl/lvgl.git third_party/lvgl
    fi
fi
echo "deps ok"
