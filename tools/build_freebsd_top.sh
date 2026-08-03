#!/usr/bin/env bash
# Cross-build the pinned FreeBSD top frontend with the generic ArmOS backend.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT_DIR="$ROOT_DIR/userland/opt/freebsd-top"
TOP_SRC="$PORT_DIR/src/usr.bin/top"
COMPAT_DIR="$PORT_DIR/compat"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/freebsd-top}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
ARCH="${ARCH:-arm-none-eabi-}"

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
NCURSES_ROOT="${NCURSES_ROOT:-$USERFS_ROOT/opt/ncurses}"

if [ ! -f "$TOP_SRC/top.c" ] || [ ! -f "$COMPAT_DIR/machine_armos.c" ]; then
    echo "error: FreeBSD top source import or ArmOS backend is incomplete" >&2
    exit 1
fi
if [ ! -f "$NEWLIB_LIBC" ] || [ ! -f "$NEWLIB_RUNTIME_DIR/crt0_newlib.o" ]; then
    echo "error: target newlib and runtime objects must be built first" >&2
    exit 1
fi
if [ ! -f "$NCURSES_ROOT/lib/libncurses.a" ]; then
    echo "error: FreeBSD top requires the target ncurses bundle" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_ROOT/usr/bin" \
    "$BUNDLE_ROOT/opt/freebsd-top/share/licenses"

CFLAGS="$ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -DARM_OS_NEWLIB -D__ARMOS__ -I$COMPAT_DIR -I$TOP_SRC -I$ROOT_DIR/userland/include -I$ROOT_DIR/include -I$NEWLIB_SYSROOT/include -I$NCURSES_ROOT/include -I$NCURSES_ROOT/include/ncurses -include $COMPAT_DIR/armos_top_compat.h"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition"
objects=()

compile_source()
{
    local name="$1"
    local source="$2"
    local object="$BUILD_DIR/$name.o"

    "$CC" $CFLAGS -c "$source" -o "$object"
    objects+=("$object")
}

for source in commands display screen top username; do
    compile_source "$source" "$TOP_SRC/$source.c"
done
for source in armos_top_compat armos_top_utils machine_armos; do
    compile_source "$source" "$COMPAT_DIR/$source.c"
done

"$CC" $LDFLAGS -o "$BUNDLE_ROOT/usr/bin/top" \
    $RUNTIME_OBJECTS "${objects[@]}" \
    "$NCURSES_ROOT/lib/libncurses.a" "$NEWLIB_LIBM" "$NEWLIB_LIBC" "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_ROOT/usr/bin/top" || true
cp "$PORT_DIR/LICENSES/FREEBSD-COPYRIGHT" \
    "$BUNDLE_ROOT/opt/freebsd-top/share/licenses/COPYRIGHT"

echo "FreeBSD top bundle ready for $TARGET_ARCH/$TARGET_PLATFORM"
