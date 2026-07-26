#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/prepare_target_userfs.sh
# Layer: Host tooling / target filesystem staging
#
# Responsibilities:
# - Create one root filesystem staging tree per architecture and platform.
# - Copy architecture-neutral seed files without importing generated binaries.
# - Keep concurrent cross-builds away from the repository userfs seed.
#
# Notes:
# - userfs/ is the immutable seed hierarchy during normal builds.
# - Generated programs, libraries and bundles live below build/<arch>/<platform>.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET_ARCH="${TARGET_ARCH:-arm32}"
TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"
SOURCE_ROOT="${USERFS_SOURCE_ROOT:-$ROOT_DIR/userfs}"
DEST_ROOT="${USERFS_ROOT:-$ROOT_DIR/build/$TARGET_ARCH/$TARGET_PLATFORM/userfs}"
STAMP="${USERFS_STAMP:-$(dirname "$DEST_ROOT")/.armos-userfs-seed-v1}"
LEGACY_STAMP="$DEST_ROOT/.armos-userfs-seed-v1"

if [ "$SOURCE_ROOT" = "$DEST_ROOT" ]; then
    echo "error: target userfs must differ from the source seed" >&2
    exit 2
fi
if [ ! -d "$SOURCE_ROOT" ]; then
    echo "error: userfs seed not found: $SOURCE_ROOT" >&2
    exit 1
fi

mkdir -p "$DEST_ROOT"
if [ "$STAMP" != "$LEGACY_STAMP" ]; then
    rm -f "$LEGACY_STAMP"
fi

for entry in Makefile README.TXT dev etc home root lib; do
    if [ -e "$SOURCE_ROOT/$entry" ]; then
        rsync -a "$SOURCE_ROOT/$entry" "$DEST_ROOT/"
    fi
done

if [ -d "$SOURCE_ROOT/usr/src" ]; then
    mkdir -p "$DEST_ROOT/usr"
    rsync -a "$SOURCE_ROOT/usr/src" "$DEST_ROOT/usr/"
fi

mkdir -p \
    "$DEST_ROOT/bin" \
    "$DEST_ROOT/dev" \
    "$DEST_ROOT/opt" \
    "$DEST_ROOT/sbin" \
    "$DEST_ROOT/tmp" \
    "$DEST_ROOT/usr/bin" \
    "$DEST_ROOT/usr/sbin"

: > "$STAMP"
