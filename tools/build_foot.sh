#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_foot.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the upstream Foot and Wayland protocol sources.
# - Generate client protocol bindings with a native wayland-scanner.
# - Cross-build a static ArmOS Foot executable and runtime configuration.
#
# Notes:
# - Upstream Foot sources are compiled without local source modifications.
# - Generated objects remain below build/<arch>/<platform>/bundles/foot.
# - Native Meson/Ninja generators come from the pinned host-tools prefix.
# - The first ArmOS port disables IME at runtime until the compositor exposes it.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FOOT_VERSION="${FOOT_VERSION:-1.9.2}"
WAYLAND_VERSION="${WAYLAND_VERSION:-1.23.1}"
WAYLAND_PROTOCOLS_VERSION="${WAYLAND_PROTOCOLS_VERSION:-1.20}"

FOOT_ARCHIVE="foot-${FOOT_VERSION}.tar.gz"
WAYLAND_ARCHIVE="wayland-${WAYLAND_VERSION}.tar.gz"
PROTOCOLS_ARCHIVE="wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}.tar.gz"
FOOT_URL="${FOOT_URL:-https://codeberg.org/dnkl/foot/archive/${FOOT_VERSION}.tar.gz}"
WAYLAND_URL="${WAYLAND_URL:-https://gitlab.freedesktop.org/wayland/wayland/-/archive/${WAYLAND_VERSION}/${WAYLAND_ARCHIVE}}"
PROTOCOLS_URL="${PROTOCOLS_URL:-https://gitlab.freedesktop.org/wayland/wayland-protocols/-/archive/${WAYLAND_PROTOCOLS_VERSION}/${PROTOCOLS_ARCHIVE}}"
FOOT_SHA512="${FOOT_SHA512:-8b14443c7be64f333a46e9aa444e64b599c5e77b622545ebdad8162dc2b50c0fa0f7d48ff369b36ec792ba64ad321b82b887e4366b5a87efb475dc64b4b98c43}"
WAYLAND_SHA512="${WAYLAND_SHA512:-454a4d7cab9fb9eafe3505114e9dfafed94c985fb7f156eb2d67f258bf2bf8418598ab75c237aa75fabe81a811981dbc72363870f2f81ddcbfb3adfaa95d4947}"
PROTOCOLS_SHA512="${PROTOCOLS_SHA512:-56c99b1534ca12e094c0ba1a7d38e7551d38dd7dea80d1a35ae4cd60e8b28ddbd8f00374394da871bbfc91aa3a42f77ebed7d62a8fe6165684a385f2028a1bf4}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/foot}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
PROTOCOL_DIR="$BUILD_DIR/protocols"
OBJECT_DIR="$BUILD_DIR/objects"
SCANNER_BUILD_DIR="$BUILD_DIR/wayland-scanner"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/foot"
HOST_TOOLS_WORK_ROOT="${FOOT_HOST_TOOLS_WORK_ROOT:-$ROOT_DIR/build/host-tools}"
HOST_TOOLS_DOWNLOAD_DIR="${FOOT_HOST_TOOLS_DOWNLOAD_DIR:-$ROOT_DIR/build/downloads/host-tools}"

FOOT_ARCHIVE_PATH="${FOOT_ARCHIVE_PATH:-$DOWNLOAD_DIR/$FOOT_ARCHIVE}"
WAYLAND_ARCHIVE_PATH="${WAYLAND_ARCHIVE_PATH:-$DOWNLOAD_DIR/$WAYLAND_ARCHIVE}"
PROTOCOLS_ARCHIVE_PATH="${PROTOCOLS_ARCHIVE_PATH:-$DOWNLOAD_DIR/$PROTOCOLS_ARCHIVE}"
FOOT_SOURCE="$SOURCE_ROOT/foot"
WAYLAND_SOURCE="$SOURCE_ROOT/wayland-$WAYLAND_VERSION"
PROTOCOLS_SOURCE="$SOURCE_ROOT/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION"
SCANNER="$SCANNER_BUILD_DIR/src/wayland-scanner"

EPOLL_PREFIX="${EPOLL_PREFIX:-$BUNDLE_BUILD_ROOT/epoll-shim/bundle/opt/epoll-shim}"
FCFT_PREFIX="${FCFT_PREFIX:-$BUNDLE_BUILD_ROOT/fcft/bundle/opt/fcft}"
HARFBUZZ_PREFIX="${HARFBUZZ_PREFIX:-$BUNDLE_BUILD_ROOT/harfbuzz/bundle/opt/harfbuzz}"
UTF8PROC_PREFIX="${UTF8PROC_PREFIX:-$BUNDLE_BUILD_ROOT/utf8proc/bundle/opt/utf8proc}"
FONTCONFIG_PREFIX="${FONTCONFIG_PREFIX:-$BUNDLE_BUILD_ROOT/fontconfig/bundle/opt/fontconfig}"
FREETYPE_PREFIX="${FREETYPE_PREFIX:-$BUNDLE_BUILD_ROOT/freetype/bundle/opt/freetype}"
EXPAT_PREFIX="${EXPAT_PREFIX:-$BUNDLE_BUILD_ROOT/expat/bundle/opt/expat}"
PIXMAN_PREFIX="${PIXMAN_PREFIX:-$BUNDLE_BUILD_ROOT/pixman/bundle/opt/pixman}"
WAYLAND_CLIENT_LIBRARY="${WAYLAND_CLIENT_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-client.a}"
WAYLAND_CURSOR_LIBRARY="${WAYLAND_CURSOR_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-cursor.a}"
XKBCOMMON_LIBRARY="${XKBCOMMON_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libxkbcommon.a}"
THREADS_LIBRARY="${THREADS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libthreads.a}"
SYSLOG_LIBRARY="${SYSLOG_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libsyslog.a}"

CC="${ARCH}gcc"
STRIP="${ARCH}strip"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

verify_archive()
{
    local archive="$1"
    local expected="$2"
    local actual

    actual="$(shasum -a 512 "$archive" | awk '{print $1}')"
    if [ "$actual" != "$expected" ]; then
        echo "error: checksum mismatch for $archive" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

fetch_archive()
{
    local path="$1"
    local url="$2"
    local checksum="$3"

    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$path" ]; then
        curl -L --fail --output "$path" "$url"
    fi
    verify_archive "$path" "$checksum"
}

fetch_archive "$FOOT_ARCHIVE_PATH" "$FOOT_URL" "$FOOT_SHA512"
fetch_archive "$WAYLAND_ARCHIVE_PATH" "$WAYLAND_URL" "$WAYLAND_SHA512"
fetch_archive "$PROTOCOLS_ARCHIVE_PATH" "$PROTOCOLS_URL" "$PROTOCOLS_SHA512"

rm -rf "$SOURCE_ROOT" "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$SOURCE_ROOT" "$PROTOCOL_DIR" "$OBJECT_DIR" \
    "$BUNDLE_BIN" "$BUNDLE_PREFIX/share"
tar -xzf "$FOOT_ARCHIVE_PATH" -C "$SOURCE_ROOT"
tar -xzf "$WAYLAND_ARCHIVE_PATH" -C "$SOURCE_ROOT"
tar -xzf "$PROTOCOLS_ARCHIVE_PATH" -C "$SOURCE_ROOT"
patch -s -d "$FOOT_SOURCE" -p1 \
    < "$ROOT_DIR/userland/opt/foot/foot-arm-os.patch"

for required_source in main.c wayland.c render.c terminal.c LICENSE; do
    if [ ! -f "$FOOT_SOURCE/$required_source" ]; then
        echo "error: Foot source tree is incomplete: $FOOT_SOURCE" >&2
        exit 1
    fi
done

for dependency in \
    "$EPOLL_PREFIX/lib/libepoll-shim.a" \
    "$FCFT_PREFIX/lib/libfcft.a" \
    "$HARFBUZZ_PREFIX/lib/libharfbuzz.a" \
    "$UTF8PROC_PREFIX/lib/libutf8proc.a" \
    "$FONTCONFIG_PREFIX/lib/libfontconfig.a" \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$EXPAT_PREFIX/lib/libexpat.a" \
    "$PIXMAN_PREFIX/lib/libpixman-1.a" \
    "$WAYLAND_CLIENT_LIBRARY" \
    "$WAYLAND_CURSOR_LIBRARY" \
    "$XKBCOMMON_LIBRARY" \
    "$THREADS_LIBRARY" \
    "$SYSLOG_LIBRARY" \
    "$NEWLIB_LIBC" \
    "$NEWLIB_LIBM"; do
    if [ ! -f "$dependency" ]; then
        echo "error: Foot dependency is missing: $dependency" >&2
        exit 1
    fi
done

if [ -n "${FOOT_HOST_TOOLS_PREFIX:-}" ]; then
    HOST_PREFIX="$FOOT_HOST_TOOLS_PREFIX"
elif [ -n "${MESA_HOST_TOOLS_PREFIX:-}" ]; then
    HOST_PREFIX="$MESA_HOST_TOOLS_PREFIX"
else
    HOST_PREFIX="$(
        WORK_ROOT="$HOST_TOOLS_WORK_ROOT" \
        DOWNLOAD_DIR="$HOST_TOOLS_DOWNLOAD_DIR" \
        "$ROOT_DIR/tools/bootstrap_mesa_host_tools.sh" --print-prefix
    )"
fi
HOST_PYTHON="$HOST_PREFIX/python/bin/python3"
MESON="$HOST_PREFIX/python/bin/meson"
NINJA="$HOST_PREFIX/python/bin/ninja"

if [ ! -x "$HOST_PYTHON" ] || [ ! -f "$MESON" ] || [ ! -x "$NINJA" ]; then
    echo "error: incomplete pinned host-tools prefix: $HOST_PREFIX" >&2
    exit 1
fi

export PATH="$HOST_PREFIX/bin:$HOST_PREFIX/python/bin:$PATH"

"$HOST_PYTHON" "$MESON" setup "$SCANNER_BUILD_DIR" "$WAYLAND_SOURCE" \
    -Ddocumentation=false -Ddtd_validation=false \
    -Dlibraries=false -Dtests=false
"$NINJA" -C "$SCANNER_BUILD_DIR"

generate_protocol()
{
    local name="$1"
    local xml="$2"

    "$SCANNER" client-header "$xml" "$PROTOCOL_DIR/$name.h"
    if [ "$name" != "xdg-shell" ]; then
        "$SCANNER" private-code "$xml" "$PROTOCOL_DIR/$name.c"
    fi
}

generate_protocol xdg-shell \
    "$PROTOCOLS_SOURCE/stable/xdg-shell/xdg-shell.xml"
generate_protocol xdg-decoration-unstable-v1 \
    "$PROTOCOLS_SOURCE/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
generate_protocol xdg-output-unstable-v1 \
    "$PROTOCOLS_SOURCE/unstable/xdg-output/xdg-output-unstable-v1.xml"
generate_protocol primary-selection-unstable-v1 \
    "$PROTOCOLS_SOURCE/unstable/primary-selection/primary-selection-unstable-v1.xml"
generate_protocol presentation-time \
    "$PROTOCOLS_SOURCE/stable/presentation-time/presentation-time.xml"
generate_protocol text-input-unstable-v3 \
    "$PROTOCOLS_SOURCE/unstable/text-input/text-input-unstable-v3.xml"

LC_ALL=C "$FOOT_SOURCE/generate-version.sh" \
    "$FOOT_VERSION" "$FOOT_SOURCE" "$PROTOCOL_DIR/version.h"

CFLAGS="$ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -Wno-incompatible-pointer-types \
-DARM_OS_NEWLIB -D_GNU_SOURCE=200809L \
-DFOOT_GRAPHEME_CLUSTERING=1 \
-include $ROOT_DIR/userland/opt/foot/armos-foot-config.h \
-include math.h -include armos/limits.h -include armos/pthread.h \
-include armos/signal.h -include armos/stdio.h -include armos/unistd.h \
-I$PROTOCOL_DIR -I$FOOT_SOURCE -I$ROOT_DIR/userland/include \
-I$EPOLL_PREFIX/include -I$FCFT_PREFIX/include \
-I$FONTCONFIG_PREFIX/include -I$PIXMAN_PREFIX/include/pixman-1 \
-I$UTF8PROC_PREFIX/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

FOOT_OBJECTS=()
for source in "$FOOT_SOURCE"/*.c; do
    name="$(basename "$source" .c)"
    if [ "$name" = "client" ]; then
        continue
    fi
    "$CC" $CFLAGS -c "$source" -o "$OBJECT_DIR/$name.o"
    FOOT_OBJECTS+=("$OBJECT_DIR/$name.o")
done
for source in "$PROTOCOL_DIR"/*.c; do
    name="$(basename "$source" .c)"
    "$CC" $CFLAGS -c "$source" -o "$OBJECT_DIR/$name-protocol.o"
    FOOT_OBJECTS+=("$OBJECT_DIR/$name-protocol.o")
done

"$CC" $LDFLAGS -o "$BUNDLE_BIN/foot" \
    $RUNTIME_OBJECTS \
    "${FOOT_OBJECTS[@]}" \
    "$FCFT_PREFIX/lib/libfcft.a" \
    "$HARFBUZZ_PREFIX/lib/libharfbuzz.a" \
    "$UTF8PROC_PREFIX/lib/libutf8proc.a" \
    "$FONTCONFIG_PREFIX/lib/libfontconfig.a" \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$EXPAT_PREFIX/lib/libexpat.a" \
    "$PIXMAN_PREFIX/lib/libpixman-1.a" \
    "$EPOLL_PREFIX/lib/libepoll-shim.a" \
    "$WAYLAND_CLIENT_LIBRARY" \
    "$WAYLAND_CURSOR_LIBRARY" \
    "$XKBCOMMON_LIBRARY" \
    "$THREADS_LIBRARY" \
    "$SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

cp "$BUNDLE_BIN/foot" "$BUILD_DIR/foot.debug"
"$STRIP" --strip-all "$BUNDLE_BIN/foot" || true
cp "$ROOT_DIR/userland/opt/foot/foot-arm-os.ini" \
    "$BUNDLE_PREFIX/share/foot.ini"
cp "$FOOT_SOURCE/LICENSE" "$BUNDLE_PREFIX/LICENSE"

echo
echo "ArmOS Foot bundle built:"
echo "  $BUNDLE_ROOT"
echo
echo "Stage with:"
echo "  rsync -a $BUNDLE_ROOT/ $USERFS_ROOT/"
