# Build environment for tools/image/build-image.sh (see build-docker.sh).
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git rsync xz-utils unzip fdisk e2fsprogs \
        python3 qemu-user-static binfmt-support file \
    && rm -rf /var/lib/apt/lists/*
RUN git config --system --add safe.directory /src
WORKDIR /src
ENTRYPOINT ["/src/tools/image/build-image.sh"]
