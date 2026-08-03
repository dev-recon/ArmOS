# syntax=docker/dockerfile:1.7
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/container/Dockerfile
# Layer: Host tooling / reproducible build environment
#
# Responsibilities:
# - Provide the complete Linux dependency set used to build ArmOS.
# - Install pinned ARM32 and ARM64 bare-metal cross toolchains.
# - Support native Docker hosts on both linux/amd64 and linux/arm64.
#
# Notes:
# - The source tree is mounted at /workspace and is never copied into the image.
# - The optional reference QEMU build is isolated from native host artifacts.

FROM ubuntu:24.04

ARG TARGETARCH
ARG ARM_GNU_VERSION=15.2.rel1

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    ARMOS_CONTAINER=1 \
    PATH=/opt/armos-toolchains/arm-none-eabi/bin:/opt/armos-toolchains/aarch64-none-elf/bin:/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin \
    LIBCXX_INCLUDE=/usr/include/c++/v1

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        autopoint \
        bash \
        bc \
        bison \
        build-essential \
        bzip2 \
        ca-certificates \
        clang \
        cmake \
        coreutils \
        curl \
        diffutils \
        dosfstools \
        e2fsprogs \
        file \
        findutils \
        flex \
        gawk \
        gettext \
        git \
        gperf \
        gzip \
        libc++-dev \
        libc++abi-dev \
        libdigest-sha-perl \
        libepoxy-dev \
        libexpat1-dev \
        libglib2.0-dev \
        libgmp-dev \
        libpixman-1-dev \
        libsdl2-dev \
        libslirp-dev \
        libtool \
        libtool-bin \
        libvirglrenderer-dev \
        libzstd1 \
        make \
        meson \
        mtools \
        nettle-dev \
        ncurses-bin \
        ninja-build \
        patch \
        perl \
        pkg-config \
        procps \
        python3 \
        python3-mako \
        python3-packaging \
        python3-pip \
        python3-setuptools \
        python3-venv \
        python3-yaml \
        qemu-system-arm \
        qemu-utils \
        rsync \
        sed \
        tar \
        texinfo \
        unzip \
        xz-utils \
        zip \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    case "$TARGETARCH" in \
        arm64) \
            arm_host=aarch64; \
            arm32_sha=d061559d814b205ed30c5b7c577c03317ec447ca51cd5a159d26b12a5bbeb20c; \
            arm64_sha=46195685b6aec1077e3f1b7706b43a6aa1fef4d8d3bff3a411b7dad1c5b1196b \
            ;; \
        amd64) \
            arm_host=x86_64; \
            arm32_sha=597893282ac8c6ab1a4073977f2362990184599643b4c5ee34870a8215783a16; \
            arm64_sha=66f7ce7c1bf662f589a4caf440812375f3cd8000a033ccf0971127a0726d6921 \
            ;; \
        *) \
            echo "unsupported Docker target architecture: $TARGETARCH" >&2; \
            exit 2 \
            ;; \
    esac; \
    base_url="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GNU_VERSION}/binrel"; \
    arm32_archive="arm-gnu-toolchain-${ARM_GNU_VERSION}-${arm_host}-arm-none-eabi.tar.xz"; \
    arm64_archive="arm-gnu-toolchain-${ARM_GNU_VERSION}-${arm_host}-aarch64-none-elf.tar.xz"; \
    mkdir -p /opt/armos-toolchains/arm-none-eabi \
             /opt/armos-toolchains/aarch64-none-elf; \
    curl -L --fail --retry 4 --output "/tmp/$arm32_archive" \
        "$base_url/$arm32_archive"; \
    curl -L --fail --retry 4 --output "/tmp/$arm64_archive" \
        "$base_url/$arm64_archive"; \
    echo "$arm32_sha  /tmp/$arm32_archive" | sha256sum --check --strict; \
    echo "$arm64_sha  /tmp/$arm64_archive" | sha256sum --check --strict; \
    tar -xJf "/tmp/$arm32_archive" \
        -C /opt/armos-toolchains/arm-none-eabi --strip-components=1; \
    tar -xJf "/tmp/$arm64_archive" \
        -C /opt/armos-toolchains/aarch64-none-elf --strip-components=1; \
    rm -f "/tmp/$arm32_archive" "/tmp/$arm64_archive"; \
    for binary in /opt/armos-toolchains/aarch64-none-elf/bin/aarch64-none-elf-*; do \
        suffix="${binary##*/aarch64-none-elf-}"; \
        ln -s "$binary" "/usr/local/bin/aarch64-elf-$suffix"; \
    done; \
    arm-none-eabi-gcc --version; \
    aarch64-elf-gcc --version

ENV ARMOS_CONTAINER_ARCH=${TARGETARCH}

COPY tools/container/entrypoint.sh /usr/local/bin/armos-container-entrypoint
RUN chmod 0755 /usr/local/bin/armos-container-entrypoint

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/armos-container-entrypoint"]
CMD ["./build.sh"]
