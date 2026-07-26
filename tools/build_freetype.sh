#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_freetype.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official FreeType release.
# - Cross-build its static library against the ArmOS newlib sysroot.
# - Package public headers, a default terminal font and a native test.
#
# Notes:
# - Objects stay below build/<arch>/<platform>/bundles/freetype.
# - Optional compression and shaping dependencies are deliberately disabled;
#   HarfBuzz is layered above this independently.
# - Upstream sources are compiled without local source modifications.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FREETYPE_VERSION="${FREETYPE_VERSION:-2.14.3}"
FREETYPE_ARCHIVE="freetype-${FREETYPE_VERSION}.tar.xz"
FREETYPE_URL="${FREETYPE_URL:-https://download.savannah.gnu.org/releases/freetype/$FREETYPE_ARCHIVE}"
FREETYPE_SHA256="${FREETYPE_SHA256:-36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/freetype}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/freetype"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_FONT_DIR="$BUNDLE_ROOT/usr/share/fonts/armos"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${FREETYPE_ARCHIVE_PATH:-$DOWNLOAD_DIR/$FREETYPE_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/freetype-$FREETYPE_VERSION}"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
FONT_SOURCE="$ROOT_DIR/assets/fonts/MesloLGS-NF-Regular.ttf"
FONT_TARGET="$BUNDLE_FONT_DIR/MesloLGS-NF-Regular.ttf"

verify_archive()
{
    local actual

    actual="$(shasum -a 256 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$FREETYPE_SHA256" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $FREETYPE_SHA256" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/include/freetype/freetype.h" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$FREETYPE_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xJf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/include/freetype/freetype.h" ] ||
   [ ! -f "$SRC_DIR/LICENSE.TXT" ]; then
    echo "error: FreeType source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi
if [ ! -f "$FONT_SOURCE" ]; then
    echo "error: default terminal font is missing: $FONT_SOURCE" >&2
    exit 1
fi
if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] ||
   [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/freetype2" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" "$BUNDLE_FONT_DIR" \
    "$BUNDLE_TCC_INCLUDE"

CFLAGS="$ARM_FLAGS -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB \
-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
CONFIGURE_LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,main"
TEST_LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

(
    cd "$BUILD_DIR"
    env \
        CC="$CC" \
        AR="${ARCH}ar" \
        RANLIB="${ARCH}ranlib" \
        STRIP="${ARCH}strip" \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$CONFIGURE_LDFLAGS" \
        "$SRC_DIR/configure" \
            --host="$TARGET_TRIPLET" \
            --prefix=/opt/freetype \
            --disable-shared \
            --enable-static \
            --without-zlib \
            --without-bzip2 \
            --without-png \
            --without-harfbuzz \
            --without-brotli
    make -j"${JOBS:-4}"
)

cp -R "$SRC_DIR/include/." "$BUNDLE_PREFIX/include/freetype2/"
cp "$BUILD_DIR/ftconfig.h" "$BUILD_DIR/ftmodule.h" \
    "$BUILD_DIR/ftoption.h" \
    "$BUNDLE_PREFIX/include/freetype2/"
cp "$BUILD_DIR/.libs/libfreetype.a" "$BUNDLE_PREFIX/lib/"
cp "$SRC_DIR/LICENSE.TXT" "$BUNDLE_PREFIX/"
cp "$FONT_SOURCE" "$FONT_TARGET"
mkdir -p "$BUNDLE_PREFIX/include/freetype" "$BUNDLE_TCC_INCLUDE/freetype"
cp "$ROOT_DIR/userland/include/ft2build.h" "$BUNDLE_PREFIX/include/"
cp "$ROOT_DIR/userland/include/ft2build.h" "$BUNDLE_TCC_INCLUDE/"
cp -R "$ROOT_DIR/userland/include/freetype/." \
    "$BUNDLE_PREFIX/include/freetype/"
cp -R "$ROOT_DIR/userland/include/freetype/." \
    "$BUNDLE_TCC_INCLUDE/freetype/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/freetype2.pc" <<EOF
prefix=/opt/freetype
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: FreeType 2
Description: A free, high-quality, and portable font engine.
Version: $FREETYPE_VERSION
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}
EOF

"$CC" $CFLAGS \
    -I"$BUNDLE_PREFIX/include" \
    -c "$ROOT_DIR/userland/opt/freetype/test/freetype-test.c" \
    -o "$BUILD_DIR/freetype-test.o"
"$CC" $TEST_LDFLAGS -o "$BUNDLE_USR_BIN/freetype-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/freetype-test.o" \
    "$BUNDLE_PREFIX/lib/libfreetype.a" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/freetype-test" || true

echo
echo "ArmOS FreeType bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
