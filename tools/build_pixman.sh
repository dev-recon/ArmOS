#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_pixman.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official Pixman source release when needed.
# - Cross-build a portable static Pixman library for ArmOS.
# - Package public headers, metadata and an ArmOS regression test.
#
# Notes:
# - Build objects stay below build/<arch>/<platform>/bundles/pixman.
# - Architecture-specific SIMD is deliberately deferred until the portable
#   renderer is validated on every ArmOS target.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIXMAN_VERSION="${PIXMAN_VERSION:-0.46.4}"
PIXMAN_ARCHIVE="pixman-${PIXMAN_VERSION}.tar.xz"
PIXMAN_URL="${PIXMAN_URL:-https://cairographics.org/releases/$PIXMAN_ARCHIVE}"
PIXMAN_SHA512="${PIXMAN_SHA512:-83b133e7969ba34f883f4e08dcc5d388c4397f43ce836c191c05945fe77c16ff501d531600780c12678a0d08105828a6bdeff2156b63f9c1a84087bc7f40ae9f}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/pixman}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/pixman"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${PIXMAN_ARCHIVE_PATH:-$DOWNLOAD_DIR/$PIXMAN_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/pixman-$PIXMAN_VERSION}"

CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
THREADS_LIBRARY="${THREADS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libthreads.a}"

PIXMAN_SRCS=(
    pixman.c
    pixman-access.c
    pixman-access-accessors.c
    pixman-arm.c
    pixman-bits-image.c
    pixman-combine32.c
    pixman-combine-float.c
    pixman-conical-gradient.c
    pixman-edge.c
    pixman-edge-accessors.c
    pixman-fast-path.c
    pixman-filter.c
    pixman-glyph.c
    pixman-general.c
    pixman-gradient-walker.c
    pixman-image.c
    pixman-implementation.c
    pixman-linear-gradient.c
    pixman-matrix.c
    pixman-mips.c
    pixman-noop.c
    pixman-ppc.c
    pixman-radial-gradient.c
    pixman-region16.c
    pixman-region32.c
    pixman-region64f.c
    pixman-riscv.c
    pixman-solid-fill.c
    pixman-timer.c
    pixman-trap.c
    pixman-utils.c
    pixman-x86.c
)

verify_archive()
{
    local actual

    actual="$(shasum -a 512 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$PIXMAN_SHA512" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $PIXMAN_SHA512" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/pixman/pixman.h" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$PIXMAN_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xJf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/pixman/pixman.h" ] ||
   [ ! -f "$SRC_DIR/COPYING" ]; then
    echo "error: Pixman source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] ||
   [ ! -f "$NEWLIB_LIBC" ] ||
   [ ! -f "$NEWLIB_LIBM" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    echo "hint: run ./tools/build_newlib.sh first" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_RUNTIME_DIR/crt0_newlib.o" ] ||
   [ ! -f "$NEWLIB_RUNTIME_DIR/pthread.o" ]; then
    echo "error: newlib-port runtime objects are missing" >&2
    echo "hint: rebuild the target userland first" >&2
    exit 1
fi

if [ ! -f "$THREADS_LIBRARY" ]; then
    echo "error: ArmOS C11 threads library is missing: $THREADS_LIBRARY" >&2
    echo "hint: rebuild the target userland first" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/pixman-1" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" "$BUNDLE_TCC_INCLUDE"

cat > "$BUILD_DIR/pixman-config.h" <<EOF
#ifndef PIXMAN_CONFIG_H
#define PIXMAN_CONFIG_H
#define HAVE_CONFIG_H 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_PTHREADS 1
#define PACKAGE "pixman"
#define SIZEOF_LONG $([ "$TARGET_ARCH" = arm64 ] && printf 8 || printf 4)
#endif
EOF

cat > "$BUILD_DIR/pixman-version.h" <<EOF
#ifndef PIXMAN_VERSION_H__
#define PIXMAN_VERSION_H__
#ifndef PIXMAN_H__
#error pixman-version.h should only be included by pixman.h
#endif
#define PIXMAN_VERSION_MAJOR 0
#define PIXMAN_VERSION_MINOR 46
#define PIXMAN_VERSION_MICRO 4
#define PIXMAN_VERSION_STRING "0.46.4"
#define PIXMAN_VERSION_ENCODE(major, minor, micro) \
    (((major) * 10000) + ((minor) * 100) + (micro))
#define PIXMAN_VERSION PIXMAN_VERSION_ENCODE(0, 46, 4)
#ifndef PIXMAN_API
#define PIXMAN_API
#endif
#endif
EOF

CFLAGS="$ARM_FLAGS -std=gnu99 -O2 -ffreestanding -fno-builtin \
-fno-strict-aliasing -fno-stack-protector -fvisibility=hidden \
-Wno-incompatible-pointer-types \
-DARM_OS_NEWLIB -DHAVE_CONFIG_H \
-I$BUILD_DIR -I$SRC_DIR/pixman \
-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

OBJECTS=()
for src in "${PIXMAN_SRCS[@]}"; do
    obj="$BUILD_DIR/${src%.c}.o"
    "$CC" $CFLAGS -c "$SRC_DIR/pixman/$src" -o "$obj"
    OBJECTS+=("$obj")
done

"$AR" rcs "$BUNDLE_PREFIX/lib/libpixman-1.a" "${OBJECTS[@]}"
"$RANLIB" "$BUNDLE_PREFIX/lib/libpixman-1.a"

cp "$ROOT_DIR/userland/include/pixman.h" \
    "$ROOT_DIR/userland/include/pixman-version.h" \
    "$BUNDLE_PREFIX/include/pixman-1/"
cp "$ROOT_DIR/userland/include/pixman.h" \
    "$ROOT_DIR/userland/include/pixman-version.h" \
    "$BUNDLE_TCC_INCLUDE/"
cp "$SRC_DIR/COPYING" "$BUNDLE_PREFIX/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/pixman-1.pc" <<EOF
prefix=/opt/pixman
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Pixman
Description: The Pixman pixel-manipulation library
Version: $PIXMAN_VERSION
Libs: -L\${libdir} -lpixman-1
Libs.private: -lm
Cflags: -I\${includedir}/pixman-1
EOF

TEST_SRC="$ROOT_DIR/userland/opt/pixman/test/pixman-test.c"
"$CC" $CFLAGS -c "$TEST_SRC" -o "$BUILD_DIR/pixman-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/pixman-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/pixman-test.o" \
    "$BUNDLE_PREFIX/lib/libpixman-1.a" \
    "$THREADS_LIBRARY" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

"$STRIP" --strip-all "$BUNDLE_USR_BIN/pixman-test" || true

echo
echo "ArmOS Pixman bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
