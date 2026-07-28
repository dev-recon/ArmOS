#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_expat.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official Expat release.
# - Cross-build the static XML parser required by Fontconfig.
# - Package public headers, metadata and a native parser test.
#
# Notes:
# - Objects stay below build/<arch>/<platform>/bundles/expat.
# - The build uses the portable entropy path and no platform-specific code.
# - Upstream sources are compiled without local source modifications.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXPAT_VERSION="${EXPAT_VERSION:-2.8.2}"
EXPAT_ARCHIVE="expat-${EXPAT_VERSION}.tar.xz"
EXPAT_URL="${EXPAT_URL:-https://github.com/libexpat/libexpat/releases/download/R_2_8_2/$EXPAT_ARCHIVE}"
EXPAT_SHA256="${EXPAT_SHA256:-3ad89b8588e6644bd4e49981480d48b21289eebbcd4f0a1a4afb1c29f99b6ab4}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/expat}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/expat"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${EXPAT_ARCHIVE_PATH:-$DOWNLOAD_DIR/$EXPAT_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/expat-$EXPAT_VERSION}"

CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local actual

    actual="$(shasum -a 256 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$EXPAT_SHA256" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $EXPAT_SHA256" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/lib/xmlparse.c" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$EXPAT_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xJf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/lib/xmlparse.c" ] ||
   [ ! -f "$SRC_DIR/COPYING" ]; then
    echo "error: Expat source tree is incomplete: $SRC_DIR" >&2
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

cat > "$BUILD_DIR/expat_config.h" <<EOF
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1
#define BYTEORDER 1234
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define PACKAGE "expat"
#define PACKAGE_NAME "expat"
#define PACKAGE_STRING "expat $EXPAT_VERSION"
#define PACKAGE_VERSION "$EXPAT_VERSION"
#define STDC_HEADERS 1
#define VERSION "$EXPAT_VERSION"
#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1
#define XML_POOR_ENTROPY 1
#endif
EOF

CFLAGS="$ARM_FLAGS -std=gnu99 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB -DHAVE_EXPAT_CONFIG_H \
-I$BUILD_DIR -I$SRC_DIR/lib \
-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

OBJECTS=()
for source in xmlparse.c xmlrole.c xmltok.c; do
    object="$BUILD_DIR/${source%.c}.o"
    "$CC" $CFLAGS -c "$SRC_DIR/lib/$source" -o "$object"
    OBJECTS+=("$object")
done
"$AR" rcs "$BUNDLE_PREFIX/lib/libexpat.a" "${OBJECTS[@]}"
"$RANLIB" "$BUNDLE_PREFIX/lib/libexpat.a"

cp "$ROOT_DIR/userland/include/expat.h" \
    "$ROOT_DIR/userland/include/expat_external.h" \
    "$BUNDLE_PREFIX/include/"
cp "$ROOT_DIR/userland/include/expat.h" \
    "$ROOT_DIR/userland/include/expat_external.h" \
    "$BUNDLE_TCC_INCLUDE/"
cp "$SRC_DIR/COPYING" "$BUNDLE_PREFIX/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/expat.pc" <<EOF
prefix=/opt/expat
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: expat
Description: XML parser
Version: $EXPAT_VERSION
Libs: -L\${libdir} -lexpat
Cflags: -I\${includedir}
EOF

"$CC" $CFLAGS \
    -c "$ROOT_DIR/userland/opt/expat/test/expat-test.c" \
    -o "$BUILD_DIR/expat-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/expat-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/expat-test.o" \
    "$BUNDLE_PREFIX/lib/libexpat.a" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/expat-test" || true

echo
echo "ArmOS Expat bundle built:"
echo "  $BUNDLE_ROOT"
