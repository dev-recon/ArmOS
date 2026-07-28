#!/usr/bin/env bash
# build_fbview.sh - cross-build the ArmOS framebuffer image viewer.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT_DIR/userland/programs/fbview/fbview.c"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/fbview}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
ZLIB_PREFIX="${ZLIB_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/zlib/bundle/opt/zlib}"
JPEG_PREFIX="${JPEG_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libjpeg/bundle/opt/libjpeg}"
PNG_PREFIX="${PNG_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libpng/bundle/opt/libpng}"
TIFF_PREFIX="${TIFF_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libtiff/bundle/opt/libtiff}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
CC="${ARCH}gcc"
STRIP="${ARCH}strip"

LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

if [ ! -f "$SRC" ]; then
    echo "error: fbview source not found: $SRC" >&2
    exit 1
fi

if [ ! -f "$ZLIB_PREFIX/include/zlib.h" ] ||
   [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "error: zlib bundle not found: $ZLIB_PREFIX" >&2
    echo "hint: run ./tools/build_zlib.sh first" >&2
    exit 1
fi

if [ ! -f "$JPEG_PREFIX/include/jpeglib.h" ] ||
   [ ! -f "$JPEG_PREFIX/lib/libjpeg.a" ]; then
    echo "error: libjpeg bundle not found: $JPEG_PREFIX" >&2
    echo "hint: run ./tools/build_libjpeg.sh first" >&2
    exit 1
fi

if [ ! -f "$PNG_PREFIX/include/png.h" ] ||
   [ ! -f "$PNG_PREFIX/lib/libpng.a" ]; then
    echo "error: libpng bundle not found: $PNG_PREFIX" >&2
    echo "hint: run ./tools/build_libpng.sh first" >&2
    exit 1
fi

if [ ! -f "$TIFF_PREFIX/include/tiffio.h" ] ||
   [ ! -f "$TIFF_PREFIX/lib/libtiff.a" ]; then
    echo "error: libtiff bundle not found: $TIFF_PREFIX" >&2
    echo "hint: run ./tools/build_libtiff.sh first" >&2
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
   [ ! -f "$NEWLIB_RUNTIME_DIR/syscall_raw.o" ] ||
   [ ! -f "$NEWLIB_RUNTIME_DIR/syscalls.o" ]; then
    echo "error: newlib-port runtime objects are missing" >&2
    echo "hint: make -C newlib-port NEWLIB_SYSROOT=$NEWLIB_SYSROOT" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_USR_BIN"

CFLAGS="$ARM_FLAGS -std=gnu99 -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fno-stack-protector -DARM_OS_NEWLIB -I$ROOT_DIR/userland/include -I$JPEG_PREFIX/include -I$PNG_PREFIX/include -I$TIFF_PREFIX/include -I$ZLIB_PREFIX/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition"

"$CC" $CFLAGS -c "$SRC" -o "$BUILD_DIR/fbview.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/fbview" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/fbview.o" \
    "$TIFF_PREFIX/lib/libtiff.a" \
    "$PNG_PREFIX/lib/libpng.a" \
    "$JPEG_PREFIX/lib/libjpeg.a" \
    "$ZLIB_PREFIX/lib/libz.a" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

"$STRIP" --strip-all "$BUNDLE_USR_BIN/fbview" || true

echo
echo "ArmOS fbview bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
