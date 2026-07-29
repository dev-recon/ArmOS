#!/usr/bin/env bash
# build_nano.sh - cross-build a minimal static nano bundle for ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
# shellcheck source=tools/configure_cache.sh
source "$ROOT_DIR/tools/configure_cache.sh"
CC="${ARCH}gcc"
STRIP="${ARCH}strip"
HOST_CC="${HOST_CC:-cc}"
NANO_VERSION="${NANO_VERSION:-8.7}"
NANO_PORT_REVISION="${NANO_PORT_REVISION:-2}"
NANO_URL="${NANO_URL:-https://www.nano-editor.org/dist/v8/nano-$NANO_VERSION.tar.xz}"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/nano}"
DOWNLOAD_DIR="$WORK_DIR/download"
SRC_ARCHIVE="$DOWNLOAD_DIR/nano-$NANO_VERSION.tar.xz"
SRC_DIR="$WORK_DIR/src"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/nano"
BUNDLE_BIN="$BUNDLE_PREFIX/bin"
BUNDLE_ETC="$BUNDLE_PREFIX/etc"
BUNDLE_SYNTAX="$BUNDLE_PREFIX/share/nano"

LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
NCURSES_PREFIX="${NCURSES_PREFIX:-$USERFS_ROOT/opt/ncurses}"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    echo "hint: run ./tools/build_newlib.sh first" >&2
    exit 1
fi

if [ ! -f "$NCURSES_PREFIX/include/curses.h" ] || [ ! -f "$NCURSES_PREFIX/lib/libncurses.a" ]; then
    echo "error: missing ArmOS ncurses bundle: $NCURSES_PREFIX" >&2
    echo "hint: run ./tools/build_ncurses.sh and stage its target bundle first" >&2
    exit 1
fi

mkdir -p "$DOWNLOAD_DIR"
if [ ! -f "$SRC_ARCHIVE" ]; then
    echo "=== Downloading nano $NANO_VERSION ==="
    curl -L --fail "$NANO_URL" -o "$SRC_ARCHIVE"
fi

SOURCE_CONTRACT="nano-$NANO_VERSION:$(shasum -a 256 "$SRC_ARCHIVE" | awk '{print $1}'):armos-$NANO_PORT_REVISION"
if [ ! -f "$SRC_DIR/.armos-source.contract" ] ||
   [ "$(cat "$SRC_DIR/.armos-source.contract")" != "$SOURCE_CONTRACT" ]; then
    rm -rf "$SRC_DIR" "$BUILD_DIR"
    mkdir -p "$SRC_DIR" "$BUILD_DIR"
    tar -xJf "$SRC_ARCHIVE" -C "$SRC_DIR" --strip-components=1

    # ArmOS' current printw path does not support the C99 %z length modifier.
    # Nano's line-number margin goes through mvwprintw(), so use a plain long.
    patch -d "$SRC_DIR" -p1 <<'PATCH'
diff --git a/src/winio.c b/src/winio.c
--- a/src/winio.c
+++ b/src/winio.c
@@ -2558,7 +2558,7 @@ void draw_row(int row, const char *converted, linestruct *line, size_t from_col)
 			mvwprintw(midwin, row, 0, "%*s", margin - 1, " ");
 		else
 #endif
-			mvwprintw(midwin, row, 0, "%*zd", margin - 1, line->lineno);
+			mvwprintw(midwin, row, 0, "%*ld", margin - 1, (long)line->lineno);
 		wattroff(midwin, interface_color_pair[LINE_NUMBER]);
 #ifndef NANO_TINY
 		if (line->has_anchor && (from_col == 0 || !ISSET(SOFTWRAP)))
PATCH

    # Newlib's sigaction handler is a scalar, so the portable zero
    # initializer uses one brace level.  This also keeps -Wall builds clean.
    patch -d "$SRC_DIR" -p1 <<'PATCH'
diff --git a/src/nano.c b/src/nano.c
--- a/src/nano.c
+++ b/src/nano.c
@@ -909 +909 @@ void set_up_sigwinch_handler(void)
-	struct sigaction deed = {{0}};
+	struct sigaction deed = {0};
@@ -920 +920 @@ void set_up_signal_handlers(void)
-	struct sigaction deed = {{0}};
+	struct sigaction deed = {0};
PATCH
    printf '%s\n' "$SOURCE_CONTRACT" > "$SRC_DIR/.armos-source.contract"
fi

rm -rf "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_BIN" "$BUNDLE_ETC" "$BUNDLE_SYNTAX"

cd "$BUILD_DIR"

# Keep the first port intentionally small.  nano's configure script comes from
# gnulib and normally probes a large Unix surface by executing test programs;
# for ArmOS cross-builds we pin the answers that matter for the compact
# profile.  Keep Nano's normal SIGWINCH support: the upstream tiny profile
# deliberately removes terminal resize handling.
cat > config.cache <<'CACHE'
ac_cv_func_chown=yes
ac_cv_func_fchmod=yes
ac_cv_func_fsync=yes
ac_cv_func_getcwd=yes
ac_cv_func_getdtablesize=yes
ac_cv_func_getopt_long=yes
ac_cv_func_getprogname=yes
ac_cv_func_getrlimit=yes
ac_cv_func_getuid=yes
ac_cv_func_lstat=yes
ac_cv_func_memmove=yes
ac_cv_func_memset=yes
ac_cv_func_poll=yes
ac_cv_func_select=yes
ac_cv_func_setlocale=no
ac_cv_func_snprintf=yes
ac_cv_func_stat=yes
ac_cv_func_strcasecmp=yes
ac_cv_func_strncasecmp=yes
ac_cv_func_strstr=yes
ac_cv_func_tcgetattr=yes
ac_cv_func_tcsetattr=yes
ac_cv_func_unlink=yes
ac_cv_func_vsnprintf=yes
ac_cv_header_glob_h=no
ac_cv_header_regex_h=yes
ac_cv_header_sys_resource_h=yes
ac_cv_header_sys_ioctl_h=yes
ac_cv_header_sys_select_h=yes
ac_cv_header_termios_h=yes
ac_cv_header_wchar_h=yes
ac_cv_lib_ncurses_initscr=yes
gl_cv_func_getcwd_path_max=yes
gl_cv_func_malloc_posix=yes
gl_cv_func_realloc_posix=yes
gl_cv_func_snprintf_posix=yes
gl_cv_func_vsnprintf_posix=yes
gl_cv_glob_overflows_stack=no
gl_cv_have_include_next=yes
gt_cv_func_gettext_libc=no
gt_cv_func_gettext_libintl=no
am_cv_func_iconv=no
CACHE

BUILD_TRIPLET="$("$SRC_DIR/config.guess" 2>/dev/null || echo unknown)"
NANO_CPPFLAGS="-I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include -I$NCURSES_PREFIX/include -I$NCURSES_PREFIX/include/ncurses"
NANO_CFLAGS="$ARM_FLAGS -Os -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB $NANO_CPPFLAGS"
NANO_COMPAT_SRC="$ROOT_DIR/userland/opt/nano/armos_nano_compat.c"

NANO_LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition $RUNTIME_OBJECTS"
NANO_LIBS="$BUILD_DIR/armos_nano_compat.o $NCURSES_PREFIX/lib/libncurses.a $NEWLIB_LIBC $LIBGCC"
if armos_configure_needed "$BUILD_DIR" "$BUILD_DIR/Makefile" <<EOF
bundle=nano
source=$SOURCE_CONTRACT
target_arch=$TARGET_ARCH
target_platform=$TARGET_PLATFORM
target_triplet=$TARGET_TRIPLET
cc=$CC
cc_version=$("$CC" --version | head -1)
host_cc=$HOST_CC
cflags=$NANO_CFLAGS
cppflags=$NANO_CPPFLAGS
ldflags=$NANO_LDFLAGS
libs=$NANO_LIBS
ncurses=$NCURSES_PREFIX
args=--cache-file=$BUILD_DIR/config.cache --build=$BUILD_TRIPLET --host=$TARGET_TRIPLET --prefix=/opt/nano --disable-tiny --disable-nls --disable-utf8 --disable-browser --enable-nanorc --enable-linenumbers --enable-color --disable-extra --disable-help --disable-histories --disable-justify --disable-libmagic --disable-multibuffer --disable-operatingdir --disable-speller --disable-tabcomp --disable-wordcomp --disable-wrapping
EOF
then
    "$CC" $NANO_CFLAGS -c "$NANO_COMPAT_SRC" \
        -o "$BUILD_DIR/armos_nano_compat.o"
    CC="$CC" \
    BUILD_CC="$HOST_CC" \
    CFLAGS="$NANO_CFLAGS" \
    CPPFLAGS="$NANO_CPPFLAGS" \
    NCURSES_CFLAGS="-I$NCURSES_PREFIX/include -I$NCURSES_PREFIX/include/ncurses" \
    NCURSES_LIBS="$NCURSES_PREFIX/lib/libncurses.a" \
    LDFLAGS="$NANO_LDFLAGS" \
    LIBS="$NANO_LIBS" \
    "$SRC_DIR/configure" \
    --cache-file="$BUILD_DIR/config.cache" \
    --build="$BUILD_TRIPLET" \
    --host="$TARGET_TRIPLET" \
    --prefix=/opt/nano \
    --disable-tiny \
    --disable-nls \
    --disable-utf8 \
    --disable-browser \
    --enable-nanorc \
    --enable-linenumbers \
    --enable-color \
    --disable-extra \
    --disable-help \
    --disable-histories \
    --disable-justify \
    --disable-libmagic \
    --disable-multibuffer \
    --disable-operatingdir \
    --disable-speller \
    --disable-tabcomp \
    --disable-wordcomp \
    --disable-wrapping
    armos_configure_commit "$BUILD_DIR"
fi

if [ "$NANO_COMPAT_SRC" -nt "$BUILD_DIR/armos_nano_compat.o" ]; then
    "$CC" $NANO_CFLAGS -c "$NANO_COMPAT_SRC" \
        -o "$BUILD_DIR/armos_nano_compat.o"
fi

make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
make DESTDIR="$BUNDLE_ROOT" install-exec

cp "$SRC_DIR"/syntax/*.nanorc "$BUNDLE_SYNTAX"/
printf 'include /opt/nano/share/nano/*.nanorc\n' > "$BUNDLE_ETC/nanorc"

"$STRIP" --strip-all "$BUNDLE_BIN/nano" || true

echo
echo "ArmOS nano bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
