#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_tllist.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official tllist release.
# - Package the header-only library and its metadata for Foot.
# - Build an ArmOS-native regression test.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TLLIST_VERSION="${TLLIST_VERSION:-1.1.0}"
TLLIST_ARCHIVE="tllist-${TLLIST_VERSION}.tar.gz"
TLLIST_URL="${TLLIST_URL:-https://codeberg.org/dnkl/tllist/archive/${TLLIST_VERSION}.tar.gz}"
TLLIST_SHA512="${TLLIST_SHA512:-9aade353a3ce4edf5ddc4ef85c1926343d9f88c9c8ee3994f0df89eefeb3b3e0ab168cf0c9a2ca4a858215c2a328462d4b5bf182134b5deb3b3a0e15af4006fe}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/tllist}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/tllist"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${TLLIST_ARCHIVE_PATH:-$DOWNLOAD_DIR/$TLLIST_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/tllist}"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local actual

    actual="$(shasum -a 512 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$TLLIST_SHA512" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $TLLIST_SHA512" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/tllist.h" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$TLLIST_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xzf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/tllist.h" ] || [ ! -f "$SRC_DIR/LICENSE" ]; then
    echo "error: tllist source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" "$BUNDLE_TCC_INCLUDE"

cp "$ROOT_DIR/userland/include/tllist.h" "$BUNDLE_PREFIX/include/"
cp "$ROOT_DIR/userland/include/tllist.h" "$BUNDLE_TCC_INCLUDE/"
cp "$SRC_DIR/LICENSE" "$BUNDLE_PREFIX/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/tllist.pc" <<EOF
prefix=/opt/tllist
includedir=\${prefix}/include

Name: tllist
Description: Typed linked-list header
Version: $TLLIST_VERSION
Cflags: -I\${includedir}
EOF

CFLAGS="$ARM_FLAGS -std=gnu11 -Os -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB \
-I$BUNDLE_PREFIX/include -I$ROOT_DIR/userland/include \
-I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

"$CC" $CFLAGS -c "$ROOT_DIR/userland/opt/tllist/test/tllist-test.c" \
    -o "$BUILD_DIR/tllist-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/tllist-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/tllist-test.o" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/tllist-test" || true

echo
echo "ArmOS tllist bundle built:"
echo "  $BUNDLE_ROOT"
