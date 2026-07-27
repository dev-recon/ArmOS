#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_fcft.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the last fcft 2.x release supported by Foot 1.9.2.
# - Cross-build the font rasterization, shaping and glyph-cache library.
# - Package public headers, metadata and a native regression test.
#
# Notes:
# - Objects stay below build/<arch>/<platform>/bundles/fcft.
# - fcft remains C; it consumes the cross-built HarfBuzz C API.
# - Upstream sources are compiled without local source modifications.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FCFT_VERSION="${FCFT_VERSION:-2.5.1}"
FCFT_ARCHIVE="fcft-${FCFT_VERSION}.tar.gz"
FCFT_URL="${FCFT_URL:-https://codeberg.org/dnkl/fcft/archive/${FCFT_VERSION}.tar.gz}"
FCFT_SHA512="${FCFT_SHA512:-a5f8baca67bb86cd478bca768259bc162472b95407a5ee4384466d44bdb17ba4788c80465a3bf61fd23e7c9c6b1fc4adef69283250510f646539614cbbca5ab0}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/fcft}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/fcft"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${FCFT_ARCHIVE_PATH:-$DOWNLOAD_DIR/$FCFT_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/fcft}"

FREETYPE_PREFIX="${FREETYPE_PREFIX:-$BUNDLE_BUILD_ROOT/freetype/bundle/opt/freetype}"
EXPAT_PREFIX="${EXPAT_PREFIX:-$BUNDLE_BUILD_ROOT/expat/bundle/opt/expat}"
FONTCONFIG_PREFIX="${FONTCONFIG_PREFIX:-$BUNDLE_BUILD_ROOT/fontconfig/bundle/opt/fontconfig}"
HARFBUZZ_PREFIX="${HARFBUZZ_PREFIX:-$BUNDLE_BUILD_ROOT/harfbuzz/bundle/opt/harfbuzz}"
UTF8PROC_PREFIX="${UTF8PROC_PREFIX:-$BUNDLE_BUILD_ROOT/utf8proc/bundle/opt/utf8proc}"
PIXMAN_PREFIX="${PIXMAN_PREFIX:-$BUNDLE_BUILD_ROOT/pixman/bundle/opt/pixman}"
TLLIST_PREFIX="${TLLIST_PREFIX:-$BUNDLE_BUILD_ROOT/tllist/bundle/opt/tllist}"
THREADS_LIBRARY="${THREADS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libthreads.a}"
SYSLOG_LIBRARY="${SYSLOG_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libsyslog.a}"

CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local actual

    actual="$(shasum -a 512 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$FCFT_SHA512" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $FCFT_SHA512" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/fcft.c" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$FCFT_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xzf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/fcft.c" ] ||
   [ ! -f "$SRC_DIR/LICENSE" ]; then
    echo "error: fcft source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi

for dependency in \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$EXPAT_PREFIX/lib/libexpat.a" \
    "$FONTCONFIG_PREFIX/lib/libfontconfig.a" \
    "$HARFBUZZ_PREFIX/lib/libharfbuzz.a" \
    "$UTF8PROC_PREFIX/lib/libutf8proc.a" \
    "$PIXMAN_PREFIX/lib/libpixman-1.a" \
    "$TLLIST_PREFIX/include/tllist.h" \
    "$THREADS_LIBRARY" \
    "$SYSLOG_LIBRARY" \
    "$NEWLIB_LIBC" \
    "$NEWLIB_LIBM"; do
    if [ ! -f "$dependency" ]; then
        echo "error: fcft dependency is missing: $dependency" >&2
        exit 1
    fi
done

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/fcft" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" \
    "$BUNDLE_TCC_INCLUDE/fcft"

"$SRC_DIR/generate-unicode-precompose.sh" \
    "$SRC_DIR/unicode/UnicodeData.txt" \
    "$BUILD_DIR/unicode-compose-table.h"
python3 "$SRC_DIR/generate-emoji-data.py" \
    "$SRC_DIR/unicode/emoji-data.txt" \
    "$BUILD_DIR/emoji-data.h"
"$SRC_DIR/generate-version.sh" "$FCFT_VERSION" "$SRC_DIR" \
    "$BUILD_DIR/version.h"

CFLAGS="$ARM_FLAGS -std=gnu18 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB -D_GNU_SOURCE=200809L \
-DFCFT_EXPORT= -DFCFT_HAVE_HARFBUZZ -DFCFT_HAVE_UTF8PROC \
-I$BUILD_DIR -I$SRC_DIR -I$ROOT_DIR/userland/include \
-I$HARFBUZZ_PREFIX/include/harfbuzz -I$UTF8PROC_PREFIX/include \
-I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

"$CC" $CFLAGS -c "$SRC_DIR/fcft.c" -o "$BUILD_DIR/fcft.o"
"$CC" $CFLAGS -c "$SRC_DIR/log.c" -o "$BUILD_DIR/log.o"
"$AR" rcs "$BUNDLE_PREFIX/lib/libfcft.a" \
    "$BUILD_DIR/fcft.o" "$BUILD_DIR/log.o"
"$RANLIB" "$BUNDLE_PREFIX/lib/libfcft.a"

cp "$ROOT_DIR/userland/include/fcft/fcft.h" \
    "$ROOT_DIR/userland/include/fcft/stride.h" \
    "$BUNDLE_PREFIX/include/fcft/"
cp -R "$ROOT_DIR/userland/include/fcft/." "$BUNDLE_TCC_INCLUDE/fcft/"
cp "$SRC_DIR/LICENSE" "$BUNDLE_PREFIX/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/fcft.pc" <<EOF
prefix=/opt/fcft
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: fcft
Description: Simple font loading and glyph rasterization library
Version: $FCFT_VERSION
Requires.private: fontconfig, freetype2, harfbuzz, libutf8proc, pixman-1, tllist
Libs: -L\${libdir} -lfcft
Libs.private: -lm -lthreads
Cflags: -I\${includedir}
EOF

"$CC" $CFLAGS \
    -c "$ROOT_DIR/userland/opt/fcft/test/fcft-test.c" \
    -o "$BUILD_DIR/fcft-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/fcft-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/fcft-test.o" \
    "$BUNDLE_PREFIX/lib/libfcft.a" \
    "$HARFBUZZ_PREFIX/lib/libharfbuzz.a" \
    "$UTF8PROC_PREFIX/lib/libutf8proc.a" \
    "$FONTCONFIG_PREFIX/lib/libfontconfig.a" \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$EXPAT_PREFIX/lib/libexpat.a" \
    "$PIXMAN_PREFIX/lib/libpixman-1.a" \
    "$THREADS_LIBRARY" \
    "$SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/fcft-test" || true

echo
echo "ArmOS fcft bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
