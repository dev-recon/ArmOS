#!/usr/bin/env bash
# Cross-build the pinned FreeBSD sh sources for ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT_DIR="$ROOT_DIR/userland/opt/freebsd-sh"
SH_SRC="$PORT_DIR/src/bin/sh"
COMPAT_DIR="$PORT_DIR/compat"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/freebsd-sh}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/freebsd-sh"
ARCH="${ARCH:-arm-none-eabi-}"

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
HOSTCC="${HOSTCC:-cc}"
HOST_MKTEMP="${HOST_MKTEMP:-$(command -v mktemp || true)}"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
WITH_LIBEDIT="${FREEBSD_SH_WITH_LIBEDIT:-1}"
INSTALL_UPSTREAM_TESTS="${FREEBSD_SH_INSTALL_UPSTREAM_TESTS:-0}"
LIBEDIT_ROOT="${LIBEDIT_ROOT:-$USERFS_ROOT/opt/libedit}"
NCURSES_ROOT="${NCURSES_ROOT:-$USERFS_ROOT/opt/ncurses}"

if [ ! -f "$SH_SRC/main.c" ] || [ ! -f "$SH_SRC/nodetypes" ]; then
    echo "error: FreeBSD sh source import is incomplete" >&2
    exit 1
fi
if [ ! -f "$NEWLIB_LIBC" ] || [ ! -f "$NEWLIB_RUNTIME_DIR/crt0_newlib.o" ]; then
    echo "error: target newlib and runtime objects must be built first" >&2
    exit 1
fi
if [ -z "$HOST_MKTEMP" ] || [ ! -x "$HOST_MKTEMP" ]; then
    echo "error: native FreeBSD sh generators require host mktemp" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/bin" "$BUNDLE_PREFIX/tests"

HOST_CFLAGS="-std=gnu11 -I$SH_SRC -include $COMPAT_DIR/armos_sh_host_compat.h"
"$HOSTCC" $HOST_CFLAGS "$SH_SRC/mknodes.c" -o "$BUILD_DIR/mknodes"
"$HOSTCC" $HOST_CFLAGS "$SH_SRC/mksyntax.c" -o "$BUILD_DIR/mksyntax"
(
    cd "$BUILD_DIR"
    PATH="$COMPAT_DIR/host-tools:$PATH" ARMOS_HOST_MKTEMP="$HOST_MKTEMP" \
        sh "$SH_SRC/mkbuiltins" "$SH_SRC"
    PATH="$COMPAT_DIR/host-tools:$PATH" ARMOS_HOST_MKTEMP="$HOST_MKTEMP" \
        sh "$SH_SRC/mktokens"
    ./mknodes "$SH_SRC/nodetypes" "$SH_SRC/nodes.c.pat"
    ./mksyntax
)

CFLAGS="$ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -DARM_OS_NEWLIB -D__ARMOS__ -DSHELL -I$COMPAT_DIR -I$BUILD_DIR -I$SH_SRC -I$SH_SRC/bltin -I$ROOT_DIR/userland/include -I$ROOT_DIR/include -I$NEWLIB_SYSROOT/include -include $COMPAT_DIR/armos_sh_compat.h"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition"
EDIT_LIBS=()
if [ "$WITH_LIBEDIT" = 1 ]; then
    if [ ! -f "$LIBEDIT_ROOT/lib/libedit.a" ] ||
       [ ! -f "$NCURSES_ROOT/lib/libncurses.a" ]; then
        echo "error: full FreeBSD sh requires target libedit and ncurses bundles" >&2
        exit 1
    fi
    CFLAGS="$CFLAGS -I$LIBEDIT_ROOT/include -I$NCURSES_ROOT/include -I$NCURSES_ROOT/include/ncurses"
    EDIT_LIBS+=("$LIBEDIT_ROOT/lib/libedit.a" "$NCURSES_ROOT/lib/libncurses.a")
else
    CFLAGS="$CFLAGS -DNO_HISTORY"
fi

objects=()
compile_source()
{
    local name="$1"
    local source="$2"
    local object="$BUILD_DIR/$name.o"

    "$CC" $CFLAGS -c "$source" -o "$object"
    objects+=("$object")
}

for source in alias arith_yacc arith_yylex cd error eval exec expand histedit input jobs mail main memalloc miscbltin mystring options output parser redir show trap var; do
    compile_source "$source" "$SH_SRC/$source.c"
done
compile_source echo "$SH_SRC/bltin/echo.c"
compile_source kill "$PORT_DIR/src/bin/kill/kill.c"
compile_source test "$PORT_DIR/src/bin/test/test.c"
compile_source printf "$PORT_DIR/src/usr.bin/printf/printf.c"
for source in builtins nodes syntax; do
    compile_source "generated_$source" "$BUILD_DIR/$source.c"
done
compile_source armos_sh_compat "$COMPAT_DIR/armos_sh_compat.c"

"$CC" $LDFLAGS -o "$BUNDLE_PREFIX/bin/sh" \
    $RUNTIME_OBJECTS "${objects[@]}" "${EDIT_LIBS[@]}" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_PREFIX/bin/sh" || true

mkdir -p "$BUNDLE_ROOT/bin"
ln -sfn ../opt/freebsd-sh/bin/sh "$BUNDLE_ROOT/bin/freebsd-sh"
ln -sfn ../opt/freebsd-sh/bin/sh "$BUNDLE_ROOT/bin/sh"
cp "$PORT_DIR/tests/armos-smoke.sh" "$BUNDLE_PREFIX/tests/armos-smoke.sh"
chmod 0755 "$BUNDLE_PREFIX/tests/armos-smoke.sh"
if [ "$INSTALL_UPSTREAM_TESTS" = 1 ]; then
    cp "$PORT_DIR/tests/run-upstream.sh" "$BUNDLE_PREFIX/tests/run-upstream.sh"
    cp -R "$SH_SRC/tests" "$BUNDLE_PREFIX/tests/upstream"
    (
        cd "$SH_SRC/tests"
        find . -type f -print | LC_ALL=C sort |
            sed -nE '/\.[0-9]+$/p' > "$BUNDLE_PREFIX/tests/upstream/armos-manifest"
    )
    chmod 0755 "$BUNDLE_PREFIX/tests/run-upstream.sh"
fi

echo "FreeBSD sh bundle ready for $TARGET_ARCH/$TARGET_PLATFORM"
