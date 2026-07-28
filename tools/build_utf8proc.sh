#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_utf8proc.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official utf8proc release.
# - Cross-build and package the static Unicode library for Foot.
# - Build an ArmOS-native Unicode regression test.
#
# Notes:
# - Objects stay below build/<arch>/<platform>/bundles/utf8proc.
# - Upstream sources are compiled without local source modifications.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
UTF8PROC_VERSION="${UTF8PROC_VERSION:-2.11.3}"
UTF8PROC_ARCHIVE="utf8proc-${UTF8PROC_VERSION}.tar.gz"
UTF8PROC_URL="${UTF8PROC_URL:-https://github.com/JuliaStrings/utf8proc/releases/download/v${UTF8PROC_VERSION}/${UTF8PROC_ARCHIVE}}"
UTF8PROC_SHA256="${UTF8PROC_SHA256:-415189fd2c85cd6ee5ff26af500fa387de9ada1e3e316e93f7338551481d557d}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/utf8proc}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/utf8proc"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${UTF8PROC_ARCHIVE_PATH:-$DOWNLOAD_DIR/$UTF8PROC_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/utf8proc-$UTF8PROC_VERSION}"

CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local actual

    actual="$(shasum -a 256 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$UTF8PROC_SHA256" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $UTF8PROC_SHA256" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/utf8proc.c" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$UTF8PROC_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xzf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/utf8proc.c" ] ||
   [ ! -f "$SRC_DIR/utf8proc_data.c" ] ||
   [ ! -f "$SRC_DIR/utf8proc.h" ] ||
   [ ! -f "$SRC_DIR/LICENSE.md" ]; then
    echo "error: utf8proc source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi
if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] ||
   [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" \
    "$BUNDLE_TCC_INCLUDE"

CFLAGS="$ARM_FLAGS -std=gnu99 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -Wno-incompatible-pointer-types -DARM_OS_NEWLIB \
-I$SRC_DIR -I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

"$CC" $CFLAGS -c "$SRC_DIR/utf8proc.c" -o "$BUILD_DIR/utf8proc.o"
"$AR" rcs "$BUNDLE_PREFIX/lib/libutf8proc.a" "$BUILD_DIR/utf8proc.o"
"$RANLIB" "$BUNDLE_PREFIX/lib/libutf8proc.a"
cp "$ROOT_DIR/userland/include/utf8proc.h" "$BUNDLE_PREFIX/include/"
cp "$ROOT_DIR/userland/include/utf8proc.h" "$BUNDLE_TCC_INCLUDE/"
cp "$SRC_DIR/LICENSE.md" "$BUNDLE_PREFIX/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/libutf8proc.pc" <<EOF
prefix=/opt/utf8proc
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libutf8proc
Description: UTF-8 processing
Version: $UTF8PROC_VERSION
Libs: -L\${libdir} -lutf8proc
Cflags: -I\${includedir}
EOF

"$CC" $CFLAGS -c \
    "$ROOT_DIR/userland/opt/utf8proc/test/utf8proc-test.c" \
    -o "$BUILD_DIR/utf8proc-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/utf8proc-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/utf8proc-test.o" \
    "$BUNDLE_PREFIX/lib/libutf8proc.a" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/utf8proc-test" || true

echo
echo "ArmOS utf8proc bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
