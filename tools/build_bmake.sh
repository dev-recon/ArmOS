#!/usr/bin/env bash
# build_bmake.sh - cross-build BSD bmake for ArmOS.
#
# bmake's own share/mk files use the :C variable modifier, which depends on
# POSIX regex support.  ArmOS enables newlib/libc/posix for arm-none-eabi so
# these symbols come from libc.a like other userland programs.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${SRC_DIR:-$ROOT_DIR/userland/opt/bmake/src}"
OVERLAY_MK="${OVERLAY_MK:-$ROOT_DIR/userland/opt/bmake/overlays/mk}"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/bmake}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/bmake"
BUNDLE_BIN="$BUNDLE_PREFIX/bin"
BUNDLE_SHARE="$BUNDLE_PREFIX/share"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
# shellcheck source=tools/configure_cache.sh
source "$ROOT_DIR/tools/configure_cache.sh"
CC="${ARCH}gcc"
STRIP="${ARCH}strip"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

LIBGCC="${LIBGCC:-$("$CC" -print-libgcc-file-name)}"

if [ ! -f "$SRC_DIR/configure" ]; then
    echo "error: bmake sources not found in $SRC_DIR" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
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

for regex_symbol in regcomp regexec regerror regfree; do
    if ! "$ARCH"nm -g "$NEWLIB_LIBC" | grep -E " T ${regex_symbol}$" >/dev/null; then
        echo "error: newlib libc.a does not provide POSIX regex symbol: $regex_symbol" >&2
        echo "hint: run ./tools/build_newlib.sh after applying the ArmOS newlib patches" >&2
        exit 1
    fi
done

rm -rf "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_BIN" "$BUNDLE_SHARE"

BMAKE_CFLAGS="$ARM_FLAGS -Os -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"

cd "$BUILD_DIR"

BMAKE_LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition $RUNTIME_OBJECTS"
BMAKE_LIBS="$NEWLIB_LIBC $LIBGCC"
if armos_configure_needed "$BUILD_DIR" "$BUILD_DIR/Makefile.config" <<EOF
bundle=bmake
target_arch=$TARGET_ARCH
target_platform=$TARGET_PLATFORM
target_triplet=$TARGET_TRIPLET
cc=$CC
cc_version=$("$CC" --version | head -1)
cflags=$BMAKE_CFLAGS
ldflags=$BMAKE_LDFLAGS
libs=$BMAKE_LIBS
source_configure=$(shasum -a 256 "$SRC_DIR/configure" | awk '{print $1}')
args=--host=$TARGET_TRIPLET --target=$TARGET_TRIPLET --prefix=/opt/bmake --without-meta --without-filemon --with-defshell=/sbin/mash --with-default-sys-path=/opt/bmake/share/mk --with-machine=armos --with-machine_arch=arm
EOF
then
    CC="$CC" \
    CFLAGS="$BMAKE_CFLAGS" \
    LDFLAGS="$BMAKE_LDFLAGS" \
    LIBS="$BMAKE_LIBS" \
    "$SRC_DIR/configure" \
    --host="$TARGET_TRIPLET" \
    --target="$TARGET_TRIPLET" \
    --prefix=/opt/bmake \
    --without-meta \
    --without-filemon \
    --with-defshell=/sbin/mash \
    --with-default-sys-path=/opt/bmake/share/mk \
    --with-machine=armos \
    --with-machine_arch=arm
    armos_configure_commit "$BUILD_DIR"
fi

sh ./make-bootstrap.sh

cp "$BUILD_DIR/bmake" "$BUNDLE_BIN/bmake"
cp -R "$SRC_DIR/mk" "$BUNDLE_SHARE/mk"
if [ -d "$OVERLAY_MK" ]; then
    cp -R "$OVERLAY_MK"/. "$BUNDLE_SHARE/mk/"
fi
"$STRIP" --strip-all "$BUNDLE_BIN/bmake" || true

echo
echo "ArmOS bmake bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
echo "  cp $BUNDLE_BIN/bmake $USERFS_ROOT/usr/bin/bmake"
