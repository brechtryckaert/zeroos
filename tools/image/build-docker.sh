#!/bin/bash
# Build the ZeroOS image inside Docker, on any Linux or macOS machine with
# Docker (Docker Desktop on a Mac works; Apple Silicon runs it natively).
#
#   ./tools/image/build-docker.sh [build-image.sh options] [-- zeroos-setup options]
#
# The container needs --privileged for loop devices. On an x86-64 host the
# arm64 binfmt handler is registered once, system-wide, with the
# tonistiigi/binfmt image (it survives until the next reboot).
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -f third_party/lvgl/lvgl.h ] || ./tools/fetch-deps.sh
if [ "$(uname -m)" != aarch64 ] && [ "$(uname -m)" != arm64 ]; then
    docker run --privileged --rm tonistiigi/binfmt --install arm64 >/dev/null
fi
docker build -t zeroos-image-builder -f tools/image/Dockerfile tools/image
mkdir -p out
docker run --rm --privileged -v /dev:/dev -v "$PWD:/src" zeroos-image-builder --out /src/out "$@"
