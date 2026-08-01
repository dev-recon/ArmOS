#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_raylib.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the pinned Raylib source release.
# - Apply the first-party ArmOS Wayland/EGL platform backend.
# - Cross-build a static OpenGL ES 2 Raylib and its contract smoke test.
# - Keep target objects below build/<arch>/<platform>/bundles/raylib.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RAYLIB_VERSION="${RAYLIB_VERSION:-6.0}"
RAYLIB_ARCHIVE="raylib-${RAYLIB_VERSION}.tar.gz"
RAYLIB_URL="${RAYLIB_URL:-https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz}"
RAYLIB_SHA256="${RAYLIB_SHA256:-2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b}"

if [ -z "${ARCH:-}" ]; then
    case "${TARGET_ARCH:-arm64}" in
        arm32) ARCH="arm-none-eabi-" ;;
        arm64) ARCH="aarch64-elf-" ;;
        *) echo "error: unsupported TARGET_ARCH='${TARGET_ARCH:-}'" >&2; exit 2 ;;
    esac
fi
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/raylib}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
ARCHIVE_PATH="${RAYLIB_ARCHIVE_PATH:-$DOWNLOAD_DIR/$RAYLIB_ARCHIVE}"
SOURCE_DIR="$WORK_DIR/source/raylib"
OBJECT_DIR="$WORK_DIR/objects"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/raylib"
MESA_PREFIX="${MESA_PREFIX:-$BUNDLE_BUILD_ROOT/mesa/bundle/opt/mesa}"
PATCH_FILE="$ROOT_DIR/tools/patches/raylib-${RAYLIB_VERSION}/0001-arm-os-wayland-egl.patch"
SOURCE_STAMP="$WORK_DIR/.source-contract"
WAYLAND_CLIENT_LIBRARY="${WAYLAND_CLIENT_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-client.a}"
WAYLAND_EGL_LIBRARY="${WAYLAND_EGL_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-egl.a}"
XKBCOMMON_LIBRARY="${XKBCOMMON_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libxkbcommon.a}"
SYSLOG_LIBRARY="${SYSLOG_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libsyslog.a}"
VIRGL_WINSYS_LIBRARY="${VIRGL_WINSYS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libarmos-virgl-winsys.a}"

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

mkdir -p "$DOWNLOAD_DIR" "$WORK_DIR"
if [ ! -f "$ARCHIVE_PATH" ]; then
    echo "=== Downloading $RAYLIB_ARCHIVE ==="
    curl -L --fail --silent --show-error "$RAYLIB_URL" -o "$ARCHIVE_PATH.tmp"
    mv "$ARCHIVE_PATH.tmp" "$ARCHIVE_PATH"
fi
actual="$(sha256_file "$ARCHIVE_PATH")"
if [ "$actual" != "$RAYLIB_SHA256" ]; then
    echo "error: SHA-256 mismatch for $ARCHIVE_PATH" >&2
    echo "expected: $RAYLIB_SHA256" >&2
    echo "actual:   $actual" >&2
    exit 1
fi

contract="$({
    printf '%s\n' "$RAYLIB_SHA256"
    shasum -a 256 "$PATCH_FILE" \
        "$ROOT_DIR/userland/opt/raylib/rcore_armos.c" \
        "$ROOT_DIR/userland/programs/raylib-smoke/raylib-smoke.c"
} | shasum -a 256 | awk '{print $1}')"
if [ ! -f "$SOURCE_STAMP" ] || [ "$(cat "$SOURCE_STAMP")" != "$contract" ]; then
    rm -rf "$SOURCE_DIR" "$OBJECT_DIR"
    mkdir -p "$SOURCE_DIR" "$OBJECT_DIR"
    tar -xzf "$ARCHIVE_PATH" -C "$SOURCE_DIR" --strip-components=1
    patch -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"
    cp "$ROOT_DIR/userland/opt/raylib/rcore_armos.c" \
        "$SOURCE_DIR/src/platforms/rcore_armos.c"
    printf '%s\n' "$contract" > "$SOURCE_STAMP"
fi

for required in \
    "$MESA_PREFIX/include/EGL/egl.h" \
    "$MESA_PREFIX/lib/libEGL.a" "$MESA_PREFIX/lib/libGLESv2.a" \
    "$WAYLAND_CLIENT_LIBRARY" "$WAYLAND_EGL_LIBRARY" \
    "$XKBCOMMON_LIBRARY" "$VIRGL_WINSYS_LIBRARY"; do
    if [ ! -f "$required" ]; then
        echo "error: Raylib dependency missing: $required" >&2
        exit 1
    fi
done

if [ "$TARGET_ARCH" = arm64 ]; then
    ARM_FLAGS="-mcpu=cortex-a53"
    TEXT_ADDRESS=0x100000000
    ELF_MACHINE=AArch64
else
    ARM_FLAGS="-mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft"
    TEXT_ADDRESS=0x8000
    ELF_MACHINE='ARM'
fi

COMMON_FLAGS="$ARM_FLAGS -std=gnu99 -O2 -ffreestanding -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ -DPLATFORM_ARMOS -DGRAPHICS_API_OPENGL_ES2 -DSUPPORT_MODULE_RAUDIO=0 -I$ROOT_DIR/userland/include -I$ROOT_DIR/include -I$NEWLIB_SYSROOT/include -I$MESA_PREFIX/include -I$SOURCE_DIR/src -I$SOURCE_DIR/src/external"

mkdir -p "$OBJECT_DIR" "$BUNDLE_PREFIX/include" "$BUNDLE_PREFIX/lib" \
    "$BUNDLE_ROOT/usr/bin"
objects=()
for module in rcore rshapes rtextures rtext rmodels; do
    object="$OBJECT_DIR/$module.o"
    "${ARCH}gcc" $COMMON_FLAGS -c "$SOURCE_DIR/src/$module.c" -o "$object"
    objects+=("$object")
done
"${ARCH}ar" rcs "$BUNDLE_PREFIX/lib/libraylib.a" "${objects[@]}"
"${ARCH}ranlib" "$BUNDLE_PREFIX/lib/libraylib.a"
cp "$SOURCE_DIR/src/raylib.h" "$SOURCE_DIR/src/raymath.h" \
    "$SOURCE_DIR/src/rlgl.h" "$BUNDLE_PREFIX/include/"

"${ARCH}gcc" $COMMON_FLAGS -I"$BUNDLE_PREFIX/include" \
    -c "$ROOT_DIR/userland/programs/raylib-smoke/raylib-smoke.c" \
    -o "$OBJECT_DIR/raylib-smoke.o"

runtime_objects="$TARGET_BUILD_ROOT/newlib-port/crt0_newlib.o $TARGET_BUILD_ROOT/newlib-port/syscall_raw.o $TARGET_BUILD_ROOT/newlib-port/syscalls.o $TARGET_BUILD_ROOT/newlib-port/stdio_lock.o $TARGET_BUILD_ROOT/newlib-port/pthread.o $TARGET_BUILD_ROOT/newlib-port/pthread_sync.o"
"${ARCH}gcc" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TEXT_ADDRESS" -Wl,-e,_start -Wl,--gc-sections \
    -Wl,--allow-multiple-definition \
    -o "$BUNDLE_ROOT/usr/bin/raylib-smoke" \
    $runtime_objects "$OBJECT_DIR/raylib-smoke.o" \
    -Wl,--start-group "$BUNDLE_PREFIX/lib/libraylib.a" \
    "$MESA_PREFIX/lib/libEGL.a" "$MESA_PREFIX/lib/libGLESv2.a" \
    "$WAYLAND_EGL_LIBRARY" "$WAYLAND_CLIENT_LIBRARY" \
    "$XKBCOMMON_LIBRARY" "$VIRGL_WINSYS_LIBRARY" "$SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" \
    "$("${ARCH}gcc" $ARM_FLAGS -print-libgcc-file-name)" \
    -Wl,--end-group

if ! "${ARCH}readelf" -h "$BUNDLE_ROOT/usr/bin/raylib-smoke" | grep -q "Machine:.*$ELF_MACHINE"; then
    echo "error: Raylib smoke test has the wrong target architecture" >&2
    exit 1
fi
echo "Raylib $RAYLIB_VERSION bundle ready for $TARGET_ARCH/$TARGET_PLATFORM"
