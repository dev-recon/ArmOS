#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_fontconfig.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the official Fontconfig release.
# - Cross-build its static library over FreeType and Expat.
# - Install a minimal ArmOS font policy and native matching test.
#
# Notes:
# - Objects stay below build/<arch>/<platform>/bundles/fontconfig.
# - Host utilities and cache generation are intentionally excluded.
# - Upstream sources are compiled without local source modifications.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FONTCONFIG_VERSION="${FONTCONFIG_VERSION:-2.18.2}"
FONTCONFIG_ARCHIVE="fontconfig-${FONTCONFIG_VERSION}.tar.xz"
FONTCONFIG_URL="${FONTCONFIG_URL:-https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/$FONTCONFIG_VERSION/$FONTCONFIG_ARCHIVE}"
FONTCONFIG_SHA256="${FONTCONFIG_SHA256:-cf8e6576ef0484c15079bdaf77cd9c51c464df5365814ada4d3ee7331ea31eb5}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/fontconfig}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/fontconfig"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${FONTCONFIG_ARCHIVE_PATH:-$DOWNLOAD_DIR/$FONTCONFIG_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/fontconfig-$FONTCONFIG_VERSION}"
FREETYPE_PREFIX="${FREETYPE_PREFIX:-$BUNDLE_BUILD_ROOT/freetype/bundle/opt/freetype}"
EXPAT_PREFIX="${EXPAT_PREFIX:-$BUNDLE_BUILD_ROOT/expat/bundle/opt/expat}"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local actual

    actual="$(shasum -a 256 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$FONTCONFIG_SHA256" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $FONTCONFIG_SHA256" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/src/fcinit.c" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$FONTCONFIG_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SOURCE_ROOT"
    tar -xJf "$ARCHIVE_PATH" -C "$SOURCE_ROOT"
fi

if [ ! -f "$SRC_DIR/src/fcinit.c" ] ||
   [ ! -f "$SRC_DIR/COPYING" ]; then
    echo "error: Fontconfig source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi
if [ ! -f "$FREETYPE_PREFIX/lib/libfreetype.a" ] ||
   [ ! -f "$EXPAT_PREFIX/lib/libexpat.a" ]; then
    echo "error: build FreeType and Expat before Fontconfig" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/fontconfig" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" \
    "$BUNDLE_ROOT/etc/fonts/conf.d" "$BUNDLE_ROOT/var/cache/fontconfig" \
    "$BUNDLE_TCC_INCLUDE/fontconfig"

CFLAGS="$ARM_FLAGS -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB \
-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
CPPFLAGS="-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include \
-I$FREETYPE_PREFIX/include/freetype2 -I$EXPAT_PREFIX/include"
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
        STRIP="$STRIP" \
        CC_FOR_BUILD=cc \
        PKG_CONFIG=false \
        CFLAGS="$CFLAGS" \
        CPPFLAGS="$CPPFLAGS" \
        LDFLAGS="$CONFIGURE_LDFLAGS" \
        LIBS="$NEWLIB_LIBM $NEWLIB_LIBC $LIBGCC" \
        FREETYPE_CFLAGS="-I$FREETYPE_PREFIX/include/freetype2" \
        FREETYPE_LIBS="$FREETYPE_PREFIX/lib/libfreetype.a" \
        EXPAT_CFLAGS="-I$EXPAT_PREFIX/include" \
        EXPAT_LIBS="$EXPAT_PREFIX/lib/libexpat.a" \
        ac_cv_search_opendir="none required" \
        ac_cv_func_vprintf=yes \
        ac_cv_func_mmap=yes \
        ac_cv_func_mkstemp=yes \
        ac_cv_func_mkdtemp=yes \
        ac_cv_func_getopt=yes \
        ac_cv_func_getopt_long=yes \
        ac_cv_func_rand=yes \
        ac_cv_func_random=no \
        ac_cv_func_lrand48=no \
        ac_cv_func_readlink=yes \
        ac_cv_func_lstat=yes \
        ac_cv_func_strdup=yes \
        ac_cv_func_vasprintf=yes \
        ac_cv_func_fabs=yes \
        ac_cv_func_XML_SetDoctypeDeclHandler=yes \
        ac_cv_va_copy=C99 \
        fc_cv_c99_vsnprintf=yes \
        "$SRC_DIR/configure" \
            --host="$TARGET_TRIPLET" \
            --prefix=/opt/fontconfig \
            --disable-shared \
            --enable-static \
            --disable-nls \
            --disable-docs \
            --disable-docbook \
            --disable-cache-build \
            --with-default-fonts=/usr/share/fonts/armos \
            --with-cache-dir=/var/cache/fontconfig \
            --with-baseconfigdir=/etc/fonts \
            --with-configdir=/etc/fonts/conf.d
    make -C fc-genericfamily fcgenericfamily.h
    make -C fc-const fcconst.h
    make -C fc-case fccase.h
    make -C fc-lang fclang.h
    make -C src fcalias.h fcaliastail.h fcftalias.h fcftaliastail.h \
        fcobjshash.h
    make -C src libfontconfig.la -j"${JOBS:-4}"
)

cp "$ROOT_DIR/userland/include/fontconfig/fontconfig.h" \
    "$ROOT_DIR/userland/include/fontconfig/fcfreetype.h" \
    "$BUNDLE_PREFIX/include/fontconfig/"
cp "$BUILD_DIR/src/.libs/libfontconfig.a" "$BUNDLE_PREFIX/lib/"
cp "$SRC_DIR/COPYING" "$BUNDLE_PREFIX/"
cp -R "$ROOT_DIR/userland/include/fontconfig/." \
    "$BUNDLE_TCC_INCLUDE/fontconfig/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/fontconfig.pc" <<EOF
prefix=/opt/fontconfig
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Fontconfig
Description: Font configuration and customization library
Version: $FONTCONFIG_VERSION
Requires.private: freetype2 >= 2.14.3, expat >= 2.8.2
Libs: -L\${libdir} -lfontconfig
Cflags: -I\${includedir}
EOF

cat > "$BUNDLE_ROOT/etc/fonts/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/usr/share/fonts/armos</dir>
  <cachedir>/var/cache/fontconfig</cachedir>
  <alias>
    <family>monospace</family>
    <prefer><family>MesloLGS NF</family></prefer>
  </alias>
</fontconfig>
EOF
cp "$SRC_DIR/fonts.dtd" "$BUNDLE_ROOT/etc/fonts/"

"$CC" $CFLAGS $CPPFLAGS \
    -I"$BUNDLE_PREFIX/include" \
    -c "$ROOT_DIR/userland/opt/fontconfig/test/fontconfig-test.c" \
    -o "$BUILD_DIR/fontconfig-test.o"
"$CC" $TEST_LDFLAGS -o "$BUNDLE_USR_BIN/fontconfig-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/fontconfig-test.o" \
    "$BUNDLE_PREFIX/lib/libfontconfig.a" \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$EXPAT_PREFIX/lib/libexpat.a" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/fontconfig-test" || true

echo
echo "ArmOS Fontconfig bundle built:"
echo "  $BUNDLE_ROOT"
