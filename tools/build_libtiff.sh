#!/usr/bin/env bash
# build_libtiff.sh - cross-build a static libtiff bundle for ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${SRC_DIR:-$ROOT_DIR/userland/opt/libtiff/src}"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libtiff}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/libtiff"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
ZLIB_PREFIX="${ZLIB_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/zlib/bundle/opt/zlib}"
JPEG_PREFIX="${JPEG_PREFIX:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libjpeg/bundle/opt/libjpeg}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"

LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
TIFFTEST_SRC="$ROOT_DIR/third_party/libtiff/tifftest.c"

LIBTIFF_SRCS=(
    tif_aux.c
    tif_close.c
    tif_codec.c
    tif_color.c
    tif_compress.c
    tif_dir.c
    tif_dirinfo.c
    tif_dirread.c
    tif_dirwrite.c
    tif_dumpmode.c
    tif_error.c
    tif_extension.c
    tif_fax3.c
    tif_fax3sm.c
    tif_flush.c
    tif_getimage.c
    tif_hash_set.c
    tif_jpeg.c
    tif_lzw.c
    tif_next.c
    tif_open.c
    tif_packbits.c
    tif_predict.c
    tif_print.c
    tif_read.c
    tif_strip.c
    tif_swab.c
    tif_thunder.c
    tif_tile.c
    tif_unix.c
    tif_version.c
    tif_warning.c
    tif_write.c
    tif_zip.c
)

if [ ! -f "$SRC_DIR/tiffio.h" ] || [ ! -f "$SRC_DIR/tif_config.h" ]; then
    echo "error: libtiff source not found in $SRC_DIR" >&2
    exit 1
fi

if [ ! -f "$ZLIB_PREFIX/include/zlib.h" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "error: zlib bundle not found: $ZLIB_PREFIX" >&2
    echo "hint: run ./tools/build_zlib.sh first" >&2
    exit 1
fi

if [ ! -f "$JPEG_PREFIX/include/jpeglib.h" ] || [ ! -f "$JPEG_PREFIX/lib/libjpeg.a" ]; then
    echo "error: libjpeg bundle not found: $JPEG_PREFIX" >&2
    echo "hint: run ./tools/build_libjpeg.sh first" >&2
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
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include" "$BUNDLE_PREFIX/lib" "$BUNDLE_USR_BIN"

CFLAGS="$ARM_FLAGS -std=gnu99 -Os -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -I$SRC_DIR -I$JPEG_PREFIX/include -I$ZLIB_PREFIX/include -I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition"

OBJECTS=()
for src in "${LIBTIFF_SRCS[@]}"; do
    obj="$BUILD_DIR/${src%.c}.o"
    "$CC" $CFLAGS -c "$SRC_DIR/$src" -o "$obj"
    OBJECTS+=("$obj")
done

"$AR" rcs "$BUNDLE_PREFIX/lib/libtiff.a" "${OBJECTS[@]}"
"$RANLIB" "$BUNDLE_PREFIX/lib/libtiff.a"

cp "$SRC_DIR/tiff.h" "$SRC_DIR/tiffconf.h" "$SRC_DIR/tiffio.h" "$SRC_DIR/tiffvers.h" "$BUNDLE_PREFIX/include/"

"$CC" $CFLAGS -c "$TIFFTEST_SRC" -o "$BUILD_DIR/tifftest.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/tifftest" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/tifftest.o" \
    "$BUNDLE_PREFIX/lib/libtiff.a" \
    "$JPEG_PREFIX/lib/libjpeg.a" \
    "$ZLIB_PREFIX/lib/libz.a" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

"$STRIP" --strip-all "$BUNDLE_USR_BIN/tifftest" || true

echo
echo "ArmOS libtiff bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
