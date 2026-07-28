#!/usr/bin/env bash
# build_ncurses.sh - cross-build a minimal static ncurses bundle for ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
# shellcheck source=tools/configure_cache.sh
source "$ROOT_DIR/tools/configure_cache.sh"
CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
HOST_CC="${HOST_CC:-cc}"
HOST_TIC="${HOST_TIC:-$(command -v tic)}"
NCURSES_VERSION="${NCURSES_VERSION:-6.5}"
NCURSES_URL="${NCURSES_URL:-https://ftp.gnu.org/pub/gnu/ncurses/ncurses-$NCURSES_VERSION.tar.gz}"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/ncurses}"
DOWNLOAD_DIR="$WORK_DIR/download"
SRC_ARCHIVE="$DOWNLOAD_DIR/ncurses-$NCURSES_VERSION.tar.gz"
SRC_DIR="$WORK_DIR/src"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/ncurses"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"

LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
TERMINFO_SRC="$ROOT_DIR/third_party/ncurses/armos.ti"
CURSESTEST_SRC="$ROOT_DIR/third_party/ncurses/cursestest.c"
TIC_PATH="$HOST_TIC"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# Apple's ncurses 6.0 tic cannot compile the current ncurses source with -x.
# The ArmOS fallback entries use standard capabilities, so a wrapper that
# removes only that flag is sufficient on affected hosts.
if "$HOST_TIC" -V 2>&1 | grep -q '^ncurses 6\.0\.20150808$'; then
    export ARMOS_HOST_TIC="$HOST_TIC"
    TIC_PATH="$ROOT_DIR/tools/tic_compat.sh"
fi

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    echo "hint: run ./tools/build_newlib.sh first" >&2
    exit 1
fi

if [ ! -f "$TERMINFO_SRC" ]; then
    echo "error: missing $TERMINFO_SRC" >&2
    exit 1
fi

mkdir -p "$DOWNLOAD_DIR"
if [ ! -f "$SRC_ARCHIVE" ]; then
    echo "=== Downloading ncurses $NCURSES_VERSION ==="
    curl -L --fail "$NCURSES_URL" -o "$SRC_ARCHIVE"
fi

SOURCE_CONTRACT="ncurses-$NCURSES_VERSION:$(shasum -a 256 "$SRC_ARCHIVE" "$TERMINFO_SRC" | shasum -a 256 | awk '{print $1}')"
if [ ! -f "$SRC_DIR/.armos-source.contract" ] ||
   [ "$(cat "$SRC_DIR/.armos-source.contract")" != "$SOURCE_CONTRACT" ]; then
    rm -rf "$SRC_DIR" "$BUILD_DIR"
    mkdir -p "$SRC_DIR" "$BUILD_DIR"
    tar -xzf "$SRC_ARCHIVE" -C "$SRC_DIR" --strip-components=1
    # Make the ArmOS entry visible to ncurses' fallback generator.
    cat "$TERMINFO_SRC" >> "$SRC_DIR/misc/terminfo.src"
    printf '%s\n' "$SOURCE_CONTRACT" > "$SRC_DIR/.armos-source.contract"
fi

rm -rf "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX" "$BUNDLE_USR_BIN"

cd "$BUILD_DIR"

BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo unknown)"

NCURSES_CFLAGS="$ARM_FLAGS -Os -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
NCURSES_CPPFLAGS="-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
NCURSES_LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition $RUNTIME_OBJECTS"
NCURSES_LIBS="$NEWLIB_LIBC $LIBGCC"
if armos_configure_needed "$BUILD_DIR" "$BUILD_DIR/Makefile" <<EOF
bundle=ncurses
source=$SOURCE_CONTRACT
target_arch=$TARGET_ARCH
target_platform=$TARGET_PLATFORM
target_triplet=$TARGET_TRIPLET
cc=$CC
cc_version=$("$CC" --version | head -1)
host_cc=$HOST_CC
host_tic=$TIC_PATH
cflags=$NCURSES_CFLAGS
cppflags=$NCURSES_CPPFLAGS
ldflags=$NCURSES_LDFLAGS
libs=$NCURSES_LIBS
args=--build=$BUILD_TRIPLET --host=$TARGET_TRIPLET --target=$TARGET_TRIPLET --prefix=/opt/ncurses --with-build-cc=$HOST_CC --with-normal --without-shared --without-debug --without-profile --without-cxx --without-cxx-binding --without-ada --without-manpages --without-tests --without-progs --disable-widec --disable-database --disable-db-install --with-fallbacks=armos,ansi --with-tic-path=$TIC_PATH --without-hashed-db --without-gpm --without-dlsym
EOF
then
    CC="$CC" \
    AR="$AR" \
    RANLIB="$RANLIB" \
    BUILD_CC="$HOST_CC" \
    CFLAGS="$NCURSES_CFLAGS" \
    CPPFLAGS="$NCURSES_CPPFLAGS" \
    LDFLAGS="$NCURSES_LDFLAGS" \
    LIBS="$NCURSES_LIBS" \
    "$SRC_DIR/configure" \
    --build="$BUILD_TRIPLET" \
    --host="$TARGET_TRIPLET" \
    --target="$TARGET_TRIPLET" \
    --prefix=/opt/ncurses \
    --with-build-cc="$HOST_CC" \
    --with-normal \
    --without-shared \
    --without-debug \
    --without-profile \
    --without-cxx \
    --without-cxx-binding \
    --without-ada \
    --without-manpages \
    --without-tests \
    --without-progs \
    --disable-widec \
    --disable-database \
    --disable-db-install \
    --with-fallbacks=armos,ansi \
    --with-tic-path="$TIC_PATH" \
    --without-hashed-db \
    --without-gpm \
    --without-dlsym
    armos_configure_commit "$BUILD_DIR"
fi

make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}" libs
make DESTDIR="$BUNDLE_ROOT" install.libs install.includes

# Generated ncurses headers retain source arguments in comments, while
# ncurses*-config otherwise records host-only linker paths. Keep the installed
# development bundle reproducible and meaningful inside ArmOS.
while IFS= read -r header; do
    sed -i.bak "s|$ROOT_DIR|$ARMOS_REPRODUCIBLE_ROOT|g" "$header"
    rm -f "$header.bak"
done < <(find "$BUNDLE_PREFIX/include" -type f -print)

NCURSES_CONFIG="$BUNDLE_PREFIX/bin/ncurses${NCURSES_VERSION%%.*}-config"
if [ -f "$NCURSES_CONFIG" ]; then
    sed -i.bak \
        -e 's|^LIBS=.*$|LIBS=""|' \
        -e 's|^for opt in .*  \$LIBS$|for opt in -L$libdir $LIBS|' \
        "$NCURSES_CONFIG"
    rm -f "$NCURSES_CONFIG.bak"
fi

for header in curses.h term.h termcap.h unctrl.h panel.h menu.h form.h eti.h; do
    if [ -f "$BUNDLE_PREFIX/include/ncurses/$header" ]; then
        ln -sf "ncurses/$header" "$BUNDLE_PREFIX/include/$header"
    fi
done

mkdir -p "$BUNDLE_PREFIX/share/terminfo"
cp "$TERMINFO_SRC" "$BUNDLE_PREFIX/share/terminfo/armos.ti"

"$CC" $ARM_FLAGS -std=gnu99 -Os -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB \
    -I"$ROOT_DIR/userland/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -I"$BUNDLE_PREFIX/include" \
    -I"$BUNDLE_PREFIX/include/ncurses" \
    -c "$CURSESTEST_SRC" \
    -o "$WORK_DIR/cursestest.o"

"$CC" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TARGET_TEXT_ADDRESS" -Wl,-e,_start -Wl,--gc-sections \
    -Wl,--allow-multiple-definition \
    -o "$BUNDLE_USR_BIN/cursestest" \
    $RUNTIME_OBJECTS \
    "$WORK_DIR/cursestest.o" \
    "$BUNDLE_PREFIX/lib/libncurses.a" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

"$STRIP" --strip-all "$BUNDLE_USR_BIN/cursestest" || true

echo
echo "ArmOS ncurses bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
