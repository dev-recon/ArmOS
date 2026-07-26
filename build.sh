#!/usr/bin/env bash
# build.sh - rebuild ArmOS without launching QEMU.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=tools/armos_config.sh
source "$ROOT_DIR/tools/armos_config.sh"
armos_config_normalize

TARGET_ARCH="${TARGET_ARCH:-arm32}"
TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"
TARGET_BUILD_ROOT="$ROOT_DIR/build/$TARGET_ARCH/$TARGET_PLATFORM"
if [ "${ARMOS_BUILD_LOCK_HELD:-0}" != "1" ]; then
    export ARMOS_BUILD_LOCK_DIR="$TARGET_BUILD_ROOT/.build.lock"
    exec "$ROOT_DIR/tools/with_build_lock.sh" "$0" "$@"
fi
if [ "$TARGET_ARCH" = "arm64" ]; then
    ARCH="${ARCH:-${CROSS_COMPILE:-aarch64-elf-}}"
else
    ARCH="${ARCH:-${CROSS_COMPILE:-arm-none-eabi-}}"
fi
BUILD_XV_DEPS="${BUILD_XV_DEPS:-0}"
BUILD_ALL_USERLAND="${BUILD_ALL_USERLAND:-0}"
BUILD_NEWLIB="${BUILD_NEWLIB:-1}"
BUILD_BSD="${BUILD_BSD:-0}"

enable_complete_userland_build()
{
    BUILD_ALL_USERLAND=1
    BUILD_TCC=1
    BUILD_BSD=1
    BUILD_NCURSES=1
    BUILD_NANO=1
    BUILD_EPOLL_SHIM=1
    BUILD_PIXMAN=1
    BUILD_TLLIST=1
    BUILD_UTF8PROC=1
    BUILD_FREETYPE=1
    BUILD_EXPAT=1
    BUILD_FONTCONFIG=1
    BUILD_ZLIB=1
    BUILD_LIBJPEG=1
    BUILD_LIBPNG=1
    BUILD_LIBTIFF=1
    BUILD_FBVIEW=1
    BUILD_XV_DEPS=1
}

if [ "$BUILD_ALL_USERLAND" = "1" ]; then
    enable_complete_userland_build
fi
if [ "$BUILD_XV_DEPS" = "1" ]; then
    BUILD_TCC="${BUILD_TCC:-0}"
    BUILD_ZLIB="${BUILD_ZLIB:-1}"
    BUILD_LIBJPEG="${BUILD_LIBJPEG:-1}"
    BUILD_LIBPNG="${BUILD_LIBPNG:-1}"
    BUILD_LIBTIFF="${BUILD_LIBTIFF:-1}"
    BUILD_FBVIEW="${BUILD_FBVIEW:-1}"
else
    BUILD_TCC="${BUILD_TCC:-1}"
    BUILD_ZLIB="${BUILD_ZLIB:-0}"
    BUILD_LIBJPEG="${BUILD_LIBJPEG:-0}"
    BUILD_LIBPNG="${BUILD_LIBPNG:-0}"
    BUILD_LIBTIFF="${BUILD_LIBTIFF:-0}"
    BUILD_FBVIEW="${BUILD_FBVIEW:-0}"
fi
BUILD_NCURSES="${BUILD_NCURSES:-0}"
BUILD_NANO="${BUILD_NANO:-0}"
BUILD_EPOLL_SHIM="${BUILD_EPOLL_SHIM:-0}"
BUILD_PIXMAN="${BUILD_PIXMAN:-0}"
BUILD_TLLIST="${BUILD_TLLIST:-0}"
BUILD_UTF8PROC="${BUILD_UTF8PROC:-0}"
BUILD_FREETYPE="${BUILD_FREETYPE:-0}"
BUILD_EXPAT="${BUILD_EXPAT:-0}"
BUILD_FONTCONFIG="${BUILD_FONTCONFIG:-0}"
ENABLE_NET="${ENABLE_NET:-0}"
ENABLE_WIFI="${ENABLE_WIFI:-0}"
ENABLE_GPU="${ENABLE_GPU:-0}"
armos_config_validate "$ROOT_DIR"

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
IMAGE_SUFFIX="${TARGET_ARCH}-${TARGET_PLATFORM}"
TARGET_USERFS="$USERFS_ROOT"
TARGET_BUNDLES="$BUNDLE_BUILD_ROOT"
ZLIB_PREFIX="$TARGET_BUNDLES/zlib/bundle/opt/zlib"
JPEG_PREFIX="$TARGET_BUNDLES/libjpeg/bundle/opt/libjpeg"
PNG_PREFIX="$TARGET_BUNDLES/libpng/bundle/opt/libpng"
TIFF_PREFIX="$TARGET_BUNDLES/libtiff/bundle/opt/libtiff"
NCURSES_PREFIX="$TARGET_USERFS/opt/ncurses"
export ZLIB_PREFIX JPEG_PREFIX PNG_PREFIX TIFF_PREFIX NCURSES_PREFIX

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:$PATH"

if command -v brew >/dev/null 2>&1; then
    E2FSPROGS_PREFIX="$(brew --prefix e2fsprogs 2>/dev/null || true)"
    if [ -d "$E2FSPROGS_PREFIX/sbin" ]; then
        export PATH="$E2FSPROGS_PREFIX/sbin:$PATH"
    fi
fi
if [ -d /opt/homebrew/opt/e2fsprogs/sbin ]; then
    export PATH="/opt/homebrew/opt/e2fsprogs/sbin:$PATH"
fi
if [ -d /usr/local/opt/e2fsprogs/sbin ]; then
    export PATH="/usr/local/opt/e2fsprogs/sbin:$PATH"
fi

cd "$ROOT_DIR"

echo "=== BUILD ARMOS ==="
echo "Target: ${TARGET_ARCH}/${TARGET_PLATFORM}"

for dir in userfs userland kernel newlib-port; do
    if [ ! -d "$dir" ]; then
        echo "Error: $dir directory not found" >&2
        exit 1
    fi
done

TARGET_ARCH="$TARGET_ARCH" TARGET_PLATFORM="$TARGET_PLATFORM" \
    USERFS_ROOT="$TARGET_USERFS" ./tools/prepare_target_userfs.sh

for tool in make python3 "${ARCH}gcc" "${ARCH}ld" "${ARCH}objcopy" "${ARCH}objdump" "${ARCH}readelf" mkfs.fat mcopy mmd mke2fs debugfs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: required tool '$tool' not found in PATH" >&2
        exit 1
    fi
done

if [ "$BUILD_ALL_USERLAND" != "1" ] &&
   ! TARGET_ARCH="$TARGET_ARCH" ARCH="$ARCH" \
       ./tools/validate_userfs_arch.sh \
           --root "$TARGET_USERFS" --allow-empty --quiet; then
    echo "=== Target userfs architecture changed; rebuilding all userland ==="
    enable_complete_userland_build
fi

if [ "$BUILD_NEWLIB" = "1" ]; then
    if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] ||
       [ ! -f "$NEWLIB_SYSROOT/lib/libc.a" ] ||
       [ ! -f "$NEWLIB_SYSROOT/.armos-reproducible-paths-v1" ]; then
        echo "=== Building repo-local newlib sysroot ==="
        TARGET="$TARGET_TRIPLET" ARCH="$ARCH" \
            NEWLIB_BUILD_ROOT="$TARGET_BUILD_ROOT/newlib-build" \
            NEWLIB_INSTALL_ROOT="$TARGET_BUILD_ROOT/newlib-sysroot" \
            ./tools/build_newlib.sh
    fi
fi

echo "=== Rebuilding userland ==="
make -C userland clean \
    TARGET_ARCH="$TARGET_ARCH" \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    TARGET_BUILD_ROOT="$TARGET_BUILD_ROOT" \
    ARCH="$ARCH"
make -C userland install \
    TARGET_ARCH="$TARGET_ARCH" \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    TARGET_BUILD_ROOT="$TARGET_BUILD_ROOT" \
    USERFS_ROOT="$TARGET_USERFS" \
    NEWLIB_RUNTIME_DIR="$NEWLIB_RUNTIME_DIR" \
    BUILD_NEWLIB="$BUILD_NEWLIB" \
    ENABLE_TCC="$BUILD_TCC" \
    ARCH="$ARCH" \
    NEWLIB_SYSROOT="$NEWLIB_SYSROOT" \
    NEWLIB_LIBC="$NEWLIB_SYSROOT/lib/libc.a"

if [ "$BUILD_TCC" = "1" ]; then
    echo "=== Building native TinyCC bundle ==="
    WORK_DIR="$TARGET_BUNDLES/tcc-native" \
        ARCH="$ARCH" NEWLIB_SYSROOT="$NEWLIB_SYSROOT" \
        ./tools/build_tcc_native.sh
    rsync -a "$TARGET_BUNDLES/tcc-native/bundle/opt/tcc/" \
        "$TARGET_USERFS/opt/tcc/"
fi

if [ "$BUILD_BSD" = "1" ]; then
    echo "=== Building BSD tools bundle ==="
    WORK_DIR="$TARGET_BUNDLES/bmake" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bmake.sh
    rsync -a "$TARGET_BUNDLES/bmake/bundle/" "$TARGET_USERFS/"
    cp "$TARGET_BUNDLES/bmake/bundle/opt/bmake/bin/bmake" \
        "$TARGET_USERFS/usr/bin/bmake"
    WORK_DIR="$TARGET_BUNDLES/bsdsed" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdsed.sh
    rsync -a "$TARGET_BUNDLES/bsdsed/bundle/" "$TARGET_USERFS/"
    ln -sfn ../opt/bsdsed/bin/sed "$TARGET_USERFS/bin/sed"
    WORK_DIR="$TARGET_BUNDLES/bsdawk" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdawk.sh
    rsync -a "$TARGET_BUNDLES/bsdawk/bundle/" "$TARGET_USERFS/"
    ln -sfn ../opt/bsdawk/bin/awk "$TARGET_USERFS/bin/awk"
    WORK_DIR="$TARGET_BUNDLES/bsdinstall" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdinstall.sh
    rsync -a "$TARGET_BUNDLES/bsdinstall/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsdinstall/bin/install "$TARGET_USERFS/usr/bin/install"
    WORK_DIR="$TARGET_BUNDLES/bsdmtree" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdmtree.sh
    rsync -a "$TARGET_BUNDLES/bsdmtree/bundle/" "$TARGET_USERFS/"
    mkdir -p "$TARGET_USERFS/usr/bin" "$TARGET_USERFS/usr/sbin"
    ln -sfn ../../opt/bsdmtree/bin/mtree "$TARGET_USERFS/usr/bin/mtree"
    ln -sfn ../../opt/bsdmtree/bin/mtree "$TARGET_USERFS/usr/sbin/mtree"
    WORK_DIR="$TARGET_BUNDLES/bsdxargs" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdxargs.sh
    rsync -a "$TARGET_BUNDLES/bsdxargs/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsdxargs/bin/xargs "$TARGET_USERFS/usr/bin/xargs"
    WORK_DIR="$TARGET_BUNDLES/bsddiff" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsddiff.sh
    rsync -a "$TARGET_BUNDLES/bsddiff/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsddiff/bin/diff "$TARGET_USERFS/usr/bin/diff"
    WORK_DIR="$TARGET_BUNDLES/bsdpatch" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdpatch.sh
    rsync -a "$TARGET_BUNDLES/bsdpatch/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsdpatch/bin/patch "$TARGET_USERFS/usr/bin/patch"
    WORK_DIR="$TARGET_BUNDLES/bsdpax" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdpax.sh
    rsync -a "$TARGET_BUNDLES/bsdpax/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsdpax/bin/pax "$TARGET_USERFS/usr/bin/pax"
    ln -sfn ../../opt/bsdpax/bin/tar "$TARGET_USERFS/usr/bin/tar"
    WORK_DIR="$TARGET_BUNDLES/bsdm4" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdm4.sh
    rsync -a "$TARGET_BUNDLES/bsdm4/bundle/" "$TARGET_USERFS/"
    ln -sfn ../../opt/bsdm4/bin/m4 "$TARGET_USERFS/usr/bin/m4"
    WORK_DIR="$TARGET_BUNDLES/bsdelftools" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_bsdelftools.sh
    rsync -a "$TARGET_BUNDLES/bsdelftools/bundle/" "$TARGET_USERFS/"
    for tool in ar ranlib nm strip size; do
        ln -sfn ../../opt/bsdelftools/bin/$tool "$TARGET_USERFS/usr/bin/$tool"
    done
fi

if [ "$BUILD_ZLIB" = "1" ]; then
    echo "=== Building zlib bundle ==="
    WORK_DIR="$TARGET_BUNDLES/zlib" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_zlib.sh
    rsync -a "$TARGET_BUNDLES/zlib/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_EPOLL_SHIM" = "1" ]; then
    echo "=== Building epoll compatibility bundle ==="
    WORK_DIR="$TARGET_BUNDLES/epoll-shim" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_epoll_shim.sh
    rsync -a "$TARGET_BUNDLES/epoll-shim/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_PIXMAN" = "1" ]; then
    echo "=== Building Pixman rendering bundle ==="
    WORK_DIR="$TARGET_BUNDLES/pixman" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_pixman.sh
    rsync -a "$TARGET_BUNDLES/pixman/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_TLLIST" = "1" ]; then
    echo "=== Building tllist header bundle ==="
    WORK_DIR="$TARGET_BUNDLES/tllist" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_tllist.sh
    rsync -a "$TARGET_BUNDLES/tllist/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_UTF8PROC" = "1" ]; then
    echo "=== Building utf8proc Unicode bundle ==="
    WORK_DIR="$TARGET_BUNDLES/utf8proc" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_utf8proc.sh
    rsync -a "$TARGET_BUNDLES/utf8proc/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_FREETYPE" = "1" ]; then
    echo "=== Building FreeType font rasterizer bundle ==="
    WORK_DIR="$TARGET_BUNDLES/freetype" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_freetype.sh
    rsync -a "$TARGET_BUNDLES/freetype/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_EXPAT" = "1" ]; then
    echo "=== Building Expat XML parser bundle ==="
    WORK_DIR="$TARGET_BUNDLES/expat" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_expat.sh
    rsync -a "$TARGET_BUNDLES/expat/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_FONTCONFIG" = "1" ]; then
    echo "=== Building Fontconfig font discovery bundle ==="
    WORK_DIR="$TARGET_BUNDLES/fontconfig" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_fontconfig.sh
    rsync -a "$TARGET_BUNDLES/fontconfig/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_LIBJPEG" = "1" ]; then
    echo "=== Building libjpeg bundle ==="
    WORK_DIR="$TARGET_BUNDLES/libjpeg" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libjpeg.sh
    rsync -a "$TARGET_BUNDLES/libjpeg/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_LIBPNG" = "1" ]; then
    if [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
        echo "=== Building zlib bundle for libpng ==="
        WORK_DIR="$TARGET_BUNDLES/zlib" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_zlib.sh
    fi
    rsync -a "$TARGET_BUNDLES/zlib/bundle/" "$TARGET_USERFS/"
    echo "=== Building libpng bundle ==="
    WORK_DIR="$TARGET_BUNDLES/libpng" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libpng.sh
    rsync -a "$TARGET_BUNDLES/libpng/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_LIBTIFF" = "1" ]; then
    if [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
        echo "=== Building zlib bundle for libtiff ==="
        WORK_DIR="$TARGET_BUNDLES/zlib" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_zlib.sh
    fi
    if [ ! -f "$JPEG_PREFIX/lib/libjpeg.a" ]; then
        echo "=== Building libjpeg bundle for libtiff ==="
        WORK_DIR="$TARGET_BUNDLES/libjpeg" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libjpeg.sh
    fi
    rsync -a "$TARGET_BUNDLES/zlib/bundle/" "$TARGET_USERFS/"
    rsync -a "$TARGET_BUNDLES/libjpeg/bundle/" "$TARGET_USERFS/"
    echo "=== Building libtiff bundle ==="
    WORK_DIR="$TARGET_BUNDLES/libtiff" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libtiff.sh
    rsync -a "$TARGET_BUNDLES/libtiff/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_FBVIEW" = "1" ]; then
    if [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
        echo "=== Building zlib bundle for fbview ==="
        WORK_DIR="$TARGET_BUNDLES/zlib" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_zlib.sh
    fi
    if [ ! -f "$JPEG_PREFIX/lib/libjpeg.a" ]; then
        echo "=== Building libjpeg bundle for fbview ==="
        WORK_DIR="$TARGET_BUNDLES/libjpeg" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libjpeg.sh
    fi
    if [ ! -f "$PNG_PREFIX/lib/libpng.a" ]; then
        echo "=== Building libpng bundle for fbview ==="
        WORK_DIR="$TARGET_BUNDLES/libpng" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libpng.sh
    fi
    if [ ! -f "$TIFF_PREFIX/lib/libtiff.a" ]; then
        echo "=== Building libtiff bundle for fbview ==="
        WORK_DIR="$TARGET_BUNDLES/libtiff" ARCH="$ARCH" \
            NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_libtiff.sh
    fi
    rsync -a "$TARGET_BUNDLES/zlib/bundle/" "$TARGET_USERFS/"
    rsync -a "$TARGET_BUNDLES/libjpeg/bundle/" "$TARGET_USERFS/"
    rsync -a "$TARGET_BUNDLES/libpng/bundle/" "$TARGET_USERFS/"
    rsync -a "$TARGET_BUNDLES/libtiff/bundle/" "$TARGET_USERFS/"
    echo "=== Building fbview ==="
    WORK_DIR="$TARGET_BUNDLES/fbview" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_fbview.sh
    rsync -a "$TARGET_BUNDLES/fbview/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_NCURSES" = "1" ]; then
    echo "=== Building ncurses bundle ==="
    WORK_DIR="$TARGET_BUNDLES/ncurses" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_ncurses.sh
    rsync -a "$TARGET_BUNDLES/ncurses/bundle/" "$TARGET_USERFS/"
fi

if [ "$BUILD_NANO" = "1" ]; then
    echo "=== Building nano bundle ==="
    if [ "$BUILD_NCURSES" != "1" ] &&
       [ ! -f "$TARGET_USERFS/opt/ncurses/lib/libncurses.a" ]; then
        echo "Error: nano requires the target ncurses bundle" >&2
        echo "Hint: rerun with BUILD_NCURSES=1 BUILD_NANO=1" >&2
        exit 1
    fi
    WORK_DIR="$TARGET_BUNDLES/nano" ARCH="$ARCH" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" ./tools/build_nano.sh
    rsync -a "$TARGET_BUNDLES/nano/bundle/" "$TARGET_USERFS/"
fi

echo "=== Rebuilding kernel ==="
make platform-kernel ARCH="$ARCH" CROSS_COMPILE="$ARCH" TARGET_ARCH="$TARGET_ARCH" TARGET_PLATFORM="$TARGET_PLATFORM"

echo "=== Recreating disk image ==="
rm -f disk.img fat32.img ext2.img "build/images/disk-${IMAGE_SUFFIX}.img"
make platform-disk ARCH="$ARCH" CROSS_COMPILE="$ARCH" TARGET_ARCH="$TARGET_ARCH" TARGET_PLATFORM="$TARGET_PLATFORM"

echo "=== Validating installed userfs ELF architecture ==="
TARGET_ARCH="$TARGET_ARCH" ARCH="$ARCH" \
    ./tools/validate_userfs_arch.sh --root "$TARGET_USERFS"

echo "=== Auditing generated images for host information ==="
./tools/check_release_image_hygiene.sh \
    "build/images/kernel-${IMAGE_SUFFIX}.bin" \
    "build/images/disk-${IMAGE_SUFFIX}.img"

echo "=== BUILD DONE ==="
echo "Kernel image: build/images/kernel-${IMAGE_SUFFIX}.bin"
echo "Disk image:   build/images/disk-${IMAGE_SUFFIX}.img"
echo "Boot existing build with: ./boot.sh"
echo "Rebuild and boot with:    ./run.sh"
