#!/usr/bin/env bash
# Cross-build the pinned FreeBSD libedit sources for ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT_DIR="$ROOT_DIR/userland/opt/freebsd-sh"
SRC_DIR="$PORT_DIR/src/contrib/libedit"
COMPAT_DIR="$PORT_DIR/compat"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/libedit}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
PREFIX="$BUNDLE_ROOT/opt/libedit"
ARCH="${ARCH:-arm-none-eabi-}"

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

CC="${ARCH}gcc"
AR="${ARCH}ar"
NCURSES_ROOT="${NCURSES_ROOT:-$USERFS_ROOT/opt/ncurses}"

if [ ! -f "$NCURSES_ROOT/lib/libncurses.a" ]; then
    echo "error: libedit requires the target ncurses bundle" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$PREFIX/include" "$PREFIX/lib"

for header in vi emacs common; do
    sh "$SRC_DIR/makelist" -h "$SRC_DIR/$header.c" > "$BUILD_DIR/$header.h"
done
sh "$SRC_DIR/makelist" -fh "$BUILD_DIR/vi.h" "$BUILD_DIR/emacs.h" "$BUILD_DIR/common.h" > "$BUILD_DIR/fcns.h"
sh "$SRC_DIR/makelist" -fc "$BUILD_DIR/vi.h" "$BUILD_DIR/emacs.h" "$BUILD_DIR/common.h" > "$BUILD_DIR/func.h"
sh "$SRC_DIR/makelist" -bh "$SRC_DIR/vi.c" "$SRC_DIR/emacs.c" "$SRC_DIR/common.c" > "$BUILD_DIR/help.h"

CFLAGS="$ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -D__ARMOS__ -D__STDC_ISO_10646__=201706L -I$COMPAT_DIR -I$BUILD_DIR -I$SRC_DIR -I$ROOT_DIR/userland/include -I$ROOT_DIR/include -I$NEWLIB_SYSROOT/include -I$NCURSES_ROOT/include -I$NCURSES_ROOT/include/ncurses -include $COMPAT_DIR/armos_sh_compat.h"
objects=()
for source in chared chartype common el eln emacs filecomplete hist history historyn keymacro literal map parse prompt read refresh search sig terminal tokenizer tokenizern tty vi; do
    object="$BUILD_DIR/$source.o"
    "$CC" $CFLAGS -c "$SRC_DIR/$source.c" -o "$object"
    objects+=("$object")
done
"$AR" rcs "$PREFIX/lib/libedit.a" "${objects[@]}"
cp "$SRC_DIR/histedit.h" "$SRC_DIR/filecomplete.h" "$PREFIX/include/"

echo "libedit bundle ready for $TARGET_ARCH/$TARGET_PLATFORM"
