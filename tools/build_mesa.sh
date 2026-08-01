#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_mesa.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the pinned Mesa release.
# - Apply the ArmOS Gallium, VirGL and surfaceless EGL integration.
# - Cross-build only static EGL, GLES2 and VirGL target libraries.
# - Keep every target object below build/<arch>/<platform>/bundles/mesa.
#
# Notes:
# - Host generators come from bootstrap_mesa_host_tools.sh and never enter
#   the target userfs.
# - The EGL platform is hardware-neutral. Provider selection is negotiated by
#   the ArmOS DRM command-set contract in pipe_loader_armos.c.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MESA_VERSION="${MESA_VERSION:-25.3.6}"
MESA_ARCHIVE="mesa-${MESA_VERSION}.tar.xz"
MESA_URL="${MESA_URL:-https://archive.mesa3d.org/$MESA_ARCHIVE}"
MESA_SHA256="${MESA_SHA256:-59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206}"

if [ -z "${ARCH:-}" ]; then
    case "${TARGET_ARCH:-arm64}" in
        arm32) ARCH="arm-none-eabi-" ;;
        arm64) ARCH="aarch64-elf-" ;;
        *)
            echo "error: unsupported TARGET_ARCH='${TARGET_ARCH:-}'" >&2
            exit 2
            ;;
    esac
fi
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/mesa}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
ARCHIVE_PATH="${MESA_ARCHIVE_PATH:-$DOWNLOAD_DIR/$MESA_ARCHIVE}"
SOURCE_DIR="$WORK_DIR/source/mesa"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/mesa"
CROSS_FILE="$WORK_DIR/armos-${TARGET_ARCH}-${TARGET_PLATFORM}.ini"
SOURCE_STAMP="$WORK_DIR/.source-contract"
PATCH_FILE="$ROOT_DIR/tools/patches/mesa-${MESA_VERSION}/0001-arm-os-static-virgl-winsys.patch"
ARMOS_WINSYS_LIBRARY="${ARMOS_WINSYS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libarmos-virgl-winsys.a}"
ARMOS_WINSYS_DIR="$(dirname "$ARMOS_WINSYS_LIBRARY")"
ARMOS_SYSLOG_LIBRARY="${ARMOS_SYSLOG_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libsyslog.a}"
ARMOS_WAYLAND_CLIENT_LIBRARY="${ARMOS_WAYLAND_CLIENT_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-client.a}"
ARMOS_WAYLAND_EGL_LIBRARY="${ARMOS_WAYLAND_EGL_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libwayland-egl.a}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
MESA_RUNTIME_DIR="$WORK_DIR/runtime"

HOST_TOOLS_WORK_ROOT="${MESA_HOST_TOOLS_WORK_ROOT:-$ROOT_DIR/build/host-tools}"
HOST_TOOLS_DOWNLOAD_DIR="${MESA_HOST_TOOLS_DOWNLOAD_DIR:-$ROOT_DIR/build/downloads/host-tools}"

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "error: sha256sum or shasum is required" >&2
        return 1
    fi
}

fetch_archive()
{
    local actual

    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        echo "=== Downloading $MESA_ARCHIVE ==="
        if ! curl -L --fail --silent --show-error \
            "$MESA_URL" -o "$ARCHIVE_PATH.tmp"; then
            rm -f "$ARCHIVE_PATH.tmp"
            return 1
        fi
        mv "$ARCHIVE_PATH.tmp" "$ARCHIVE_PATH"
    fi

    actual="$(sha256_file "$ARCHIVE_PATH")"
    if [ "$actual" != "$MESA_SHA256" ]; then
        echo "error: SHA-256 mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $MESA_SHA256" >&2
        echo "actual:   $actual" >&2
        return 1
    fi
}

integration_contract()
{
    local file

    for file in \
        "$PATCH_FILE" \
        "$ROOT_DIR/userland/opt/mesa/armos/armgl_frontend.c" \
        "$ROOT_DIR/userland/opt/mesa/armos/armgl_frontend.h" \
        "$ROOT_DIR/userland/opt/mesa/armos/armgl.meson.build" \
        "$ROOT_DIR/userland/opt/mesa/armos/virgl_armos_winsys.c" \
        "$ROOT_DIR/userland/opt/mesa/armos/virgl_armos_winsys.h" \
        "$ROOT_DIR/userland/opt/mesa/armos/meson.build" \
        "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader_armos.c" \
        "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader_armos.h" \
        "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader.meson.build" \
        "$ROOT_DIR/userland/opt/mesa/armos/egl_armos.c" \
        "$ROOT_DIR/userland/opt/mesa/armos/new_armos" \
        "$ROOT_DIR/userland/opt/mesa/armos/cxx_runtime_armos.cpp" \
        "$ROOT_DIR/userland/opt/mesa/armos/lower_precision_armos.cpp" \
        "$ROOT_DIR/userland/opt/mesa/armos/wlcomp_gpu_armgl.c" \
        "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_backend.h" \
        "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_backend_provider.h" \
        "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_backend.c" \
        "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_present.h" \
        "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_present.c" \
        "$ROOT_DIR/userland/include/wayland-egl-core.h" \
        "$ROOT_DIR/userland/include/wayland-egl-backend.h" \
        "$ROOT_DIR/userland/include/wayland-armos-gpu-client-protocol.h" \
        "$ROOT_DIR/userland/lib/wayland/egl.c" \
        "$ROOT_DIR/userland/programs/egl-smoke/egl-smoke.c" \
        "$ROOT_DIR/userland/programs/egl-wayland-smoke/egl-wayland-smoke.c" \
        "$ROOT_DIR/userland/programs/armgl-import-smoke/armgl-import-smoke.c" \
        "$ROOT_DIR/userland/programs/armgl-compositor-smoke/armgl-compositor-smoke.c"; do
        sha256_file "$file"
    done | sha256_file /dev/stdin
}

prepare_source()
{
    local contract

    contract="mesa=$MESA_VERSION:$(integration_contract)"
    if [ "${ARMOS_FORCE_MESA_REBUILD:-0}" != "1" ] &&
       [ -f "$SOURCE_STAMP" ] && [ "$(cat "$SOURCE_STAMP")" = "$contract" ] &&
       [ -f "$SOURCE_DIR/src/egl/drivers/armos/egl_armos.c" ]; then
        return
    fi

    rm -rf "$SOURCE_DIR" "$BUILD_DIR" "$BUNDLE_ROOT"
    mkdir -p "$SOURCE_DIR" "$WORK_DIR/source"
    tar -xJf "$ARCHIVE_PATH" -C "$SOURCE_DIR" --strip-components=1
    patch -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"

    mkdir -p \
        "$SOURCE_DIR/src/gallium/frontends/armgl" \
        "$SOURCE_DIR/src/gallium/winsys/virgl/armos" \
        "$SOURCE_DIR/src/gallium/targets/armos" \
        "$SOURCE_DIR/src/egl/drivers/armos"
    cp "$ROOT_DIR/userland/opt/mesa/armos/armgl_frontend.c" \
       "$ROOT_DIR/userland/opt/mesa/armos/armgl_frontend.h" \
       "$SOURCE_DIR/src/gallium/frontends/armgl/"
    cp "$ROOT_DIR/userland/opt/mesa/armos/armgl.meson.build" \
       "$SOURCE_DIR/src/gallium/frontends/armgl/meson.build"
    cp "$ROOT_DIR/userland/opt/mesa/armos/virgl_armos_winsys.c" \
       "$ROOT_DIR/userland/opt/mesa/armos/virgl_armos_winsys.h" \
       "$SOURCE_DIR/src/gallium/winsys/virgl/armos/"
    cp "$ROOT_DIR/userland/opt/mesa/armos/meson.build" \
       "$SOURCE_DIR/src/gallium/winsys/virgl/armos/meson.build"
    cp "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader_armos.c" \
       "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader_armos.h" \
       "$SOURCE_DIR/src/gallium/targets/armos/"
    cp "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader.meson.build" \
       "$SOURCE_DIR/src/gallium/targets/armos/meson.build"
    cp "$ROOT_DIR/userland/opt/mesa/armos/egl_armos.c" \
       "$SOURCE_DIR/src/egl/drivers/armos/"
    cp "$ROOT_DIR/userland/opt/mesa/armos/new_armos" \
       "$SOURCE_DIR/include/new"
    cp "$ROOT_DIR/userland/opt/mesa/armos/cxx_runtime_armos.cpp" \
       "$SOURCE_DIR/src/util/armos_cxx_runtime.cpp"
    cp "$ROOT_DIR/userland/opt/mesa/armos/lower_precision_armos.cpp" \
       "$SOURCE_DIR/src/compiler/glsl/lower_precision.cpp"
    printf '%s\n' "$contract" > "$SOURCE_STAMP"
}

meson_quote()
{
    printf "'%s'" "${1//\'/\\\'}"
}

write_cross_file()
{
    local compiler_family cpu first argument
    local c_compiler cxx_compiler ar ranlib strip libgcc
    local common_args cpp_args link_args

    c_compiler="$(command -v "${ARCH}gcc")"
    cxx_compiler="$(command -v "${ARCH}g++")"
    ar="$(command -v "${ARCH}ar")"
    ranlib="$(command -v "${ARCH}ranlib")"
    strip="$(command -v "${ARCH}strip")"
    libgcc="$("$c_compiler" $ARM_FLAGS -print-libgcc-file-name)"

    if [ "$TARGET_ARCH" = "arm64" ]; then
        compiler_family="aarch64"
        cpu="cortex-a53"
    else
        compiler_family="arm"
        cpu="cortex-a15"
    fi

    common_args="$ARM_FLAGS -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
-D_GNU_SOURCE=1 \
-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
-I$ROOT_DIR/userland/include -I$ROOT_DIR/include -I$NEWLIB_SYSROOT/include"
    cpp_args="$common_args -fno-exceptions -fno-rtti"
    link_args="-nostdlib -Wl,-e,main -Wl,--allow-multiple-definition \
-L$ARMOS_WINSYS_DIR $RUNTIME_OBJECTS \
$NEWLIB_LIBM $NEWLIB_LIBC $libgcc"

    {
        echo "[binaries]"
        printf 'c = '; meson_quote "$c_compiler"; echo
        printf 'cpp = '; meson_quote "$cxx_compiler"; echo
        printf 'ar = '; meson_quote "$ar"; echo
        printf 'ranlib = '; meson_quote "$ranlib"; echo
        printf 'strip = '; meson_quote "$strip"; echo
        printf 'pkg-config = '; meson_quote /usr/bin/false; echo
        # Meson invokes the wrapper for its compiler sanity executable. true
        # records success without ever attempting to execute a target ELF.
        printf 'exe_wrapper = '; meson_quote /usr/bin/true; echo
        printf 'python = '; meson_quote "$HOST_PYTHON"; echo
        echo
        echo "[host_machine]"
        echo "system = 'armos'"
        printf 'cpu_family = '; meson_quote "$compiler_family"; echo
        printf 'cpu = '; meson_quote "$cpu"; echo
        echo "endian = 'little'"
        echo
        echo "[properties]"
        echo "needs_exe_wrapper = true"
        printf 'sys_root = '; meson_quote "$NEWLIB_SYSROOT"; echo
        echo
        echo "[built-in options]"
        printf 'c_args = ['
        first=1
        for argument in $common_args; do
            [ "$first" = 1 ] || printf ', '
            meson_quote "$argument"
            first=0
        done
        echo ']'
        printf 'cpp_args = ['
        first=1
        for argument in $cpp_args; do
            [ "$first" = 1 ] || printf ', '
            meson_quote "$argument"
            first=0
        done
        echo ']'
        printf 'c_link_args = ['
        first=1
        for argument in $link_args; do
            [ "$first" = 1 ] || printf ', '
            meson_quote "$argument"
            first=0
        done
        echo ']'
        printf 'cpp_link_args = ['
        first=1
        for argument in $link_args; do
            [ "$first" = 1 ] || printf ', '
            meson_quote "$argument"
            first=0
        done
        echo ']'
    } > "$CROSS_FILE"

}

prepare_private_runtime()
{
    echo "=== Building private ArmOS runtime objects ==="
    "${MAKE:-make}" -C "$ROOT_DIR/newlib-port" \
        TARGET_ARCH="$TARGET_ARCH" \
        TARGET_PLATFORM="$TARGET_PLATFORM" \
        ARCH="$ARCH" \
        BUILD_DIR="$MESA_RUNTIME_DIR" \
        NEWLIB_SYSROOT="$NEWLIB_SYSROOT" \
        NEWLIB_LIBC="$NEWLIB_LIBC"

    RUNTIME_OBJECTS="$MESA_RUNTIME_DIR/crt0_newlib.o \
$MESA_RUNTIME_DIR/syscall_raw.o \
$MESA_RUNTIME_DIR/syscalls.o \
$MESA_RUNTIME_DIR/stdio_lock.o \
$MESA_RUNTIME_DIR/pthread.o \
$MESA_RUNTIME_DIR/pthread_sync.o"
}

for dependency in \
    "$NEWLIB_LIBC" \
    "$NEWLIB_LIBM" \
    "$ARMOS_WINSYS_LIBRARY" \
    "$ARMOS_SYSLOG_LIBRARY" \
    "$ARMOS_WAYLAND_CLIENT_LIBRARY" \
    "$ARMOS_WAYLAND_EGL_LIBRARY" \
    "$PATCH_FILE"; do
    if [ ! -f "$dependency" ]; then
        echo "error: Mesa dependency is missing: $dependency" >&2
        exit 1
    fi
done

require_archive_symbol()
{
    local archive="$1"
    local symbol="$2"

    if ! "${ARCH}nm" -g --defined-only "$archive" 2>/dev/null |
            awk -v expected="$symbol" '$NF == expected { found = 1 } END { exit !found }'; then
        echo "error: stale or incompatible target archive: $archive" >&2
        echo "missing ABI symbol: $symbol" >&2
        echo "rebuild the target userland before Mesa" >&2
        exit 1
    fi
}

if ! command -v "${ARCH}nm" >/dev/null 2>&1; then
    echo "error: target archive inspector not found: ${ARCH}nm" >&2
    exit 1
fi
require_archive_symbol "$ARMOS_WINSYS_LIBRARY" armos_virgl_buffer_export
require_archive_symbol "$ARMOS_WINSYS_LIBRARY" armos_virgl_buffer_import
require_archive_symbol "$ARMOS_WINSYS_LIBRARY" armos_virgl_buffer_set_metadata
require_archive_symbol "$ARMOS_WINSYS_LIBRARY" armos_virgl_fence_export
require_archive_symbol "$ARMOS_WAYLAND_CLIENT_LIBRARY" wl_proxy_get_display
require_archive_symbol "$ARMOS_WAYLAND_EGL_LIBRARY" wl_egl_window_create

for tool in curl tar xz patch; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required Mesa build tool '$tool' not found" >&2
        exit 1
    fi
done

if [ -n "${MESA_HOST_TOOLS_PREFIX:-}" ]; then
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

fetch_archive
prepare_source
prepare_private_runtime
write_cross_file
rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_ROOT"

export PATH="$HOST_PREFIX/bin:$HOST_PREFIX/python/bin:$PATH"
export M4="$HOST_PREFIX/bin/m4"
export BISON="$HOST_PREFIX/bin/bison"
export PKG_CONFIG_LIBDIR=/nonexistent
export PKG_CONFIG_PATH=

"$HOST_PYTHON" "$MESON" setup "$BUILD_DIR" "$SOURCE_DIR" \
    --cross-file "$CROSS_FILE" \
    --prefix /opt/mesa \
    --buildtype release \
    --default-library static \
    -Darmos-virgl-winsys-dir="$ARMOS_WINSYS_DIR" \
    -Dplatforms=[] \
    -Degl-native-platform=surfaceless \
    -Dgallium-drivers=virgl \
    -Dvulkan-drivers=[] \
    -Dllvm=disabled \
    -Dglx=disabled \
    -Degl=enabled \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dopengl=false \
    -Dgbm=disabled \
    -Dshared-glapi=disabled \
    -Dglvnd=disabled \
    -Dshader-cache=disabled \
    -Dxmlconfig=disabled \
    -Dexpat=disabled \
    -Dzlib=disabled \
    -Dzstd=disabled \
    -Dvalgrind=disabled \
    -Dlibunwind=disabled \
    -Dlmsensors=disabled \
    -Dbuild-tests=false \
    -Denable-glcpp-tests=false \
    -Dtools=[] \
    -Dvideo-codecs=[] \
    -Dallow-kcmp=disabled \
    -Dperfetto=false \
    -Ddatasources=[]

"$NINJA" -C "$BUILD_DIR" -j "$JOBS"
DESTDIR="$BUNDLE_ROOT" "$HOST_PYTHON" "$MESON" install \
    -C "$BUILD_DIR" --no-rebuild

mkdir -p "$BUNDLE_PREFIX"
mkdir -p "$BUNDLE_PREFIX/share/licenses"
cp "$SOURCE_DIR/docs/license.rst" "$BUNDLE_PREFIX/share/licenses/README.rst"
cp -R "$SOURCE_DIR/licenses/." "$BUNDLE_PREFIX/share/licenses/"

for library in libEGL.a libGLESv2.a; do
    if [ ! -f "$BUNDLE_PREFIX/lib/$library" ]; then
        echo "error: Mesa did not install $library" >&2
        exit 1
    fi
done

if ! grep -Fq -- '-larmos-virgl-winsys' \
        "$BUNDLE_PREFIX/lib/pkgconfig/egl.pc"; then
    echo "error: Mesa EGL metadata omits the target VirGL winsys dependency" >&2
    exit 1
fi
if LC_ALL=C grep -R -a -Fq "$ROOT_DIR" \
        "$BUNDLE_PREFIX/lib/pkgconfig"; then
    echo "error: Mesa pkg-config metadata exposes the host source tree" >&2
    LC_ALL=C grep -R -a -F "$ROOT_DIR" \
        "$BUNDLE_PREFIX/lib/pkgconfig" >&2 || true
    exit 1
fi

echo "=== Building ArmOS compositor GPU provider ==="
MESA_ENUM_FLAGS=""
if [ "$TARGET_ARCH" = "arm32" ]; then
    MESA_ENUM_FLAGS="-fno-short-enums"
fi
mkdir -p "$WORK_DIR/wlcomp-gpu"
"${ARCH}gcc" $ARM_FLAGS $MESA_ENUM_FLAGS -std=gnu11 -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector \
    -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
    -Wall -Wextra -Werror \
    -I"$BUILD_DIR/src" -I"$BUILD_DIR/src/util/format" \
    -I"$ROOT_DIR/userland/opt/mesa/armos" \
    -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -I"$SOURCE_DIR/include" -I"$SOURCE_DIR/src" \
    -I"$SOURCE_DIR/src/gallium/include" \
    -c "$ROOT_DIR/userland/opt/mesa/armos/wlcomp_gpu_armgl.c" \
    -o "$WORK_DIR/wlcomp-gpu/wlcomp_gpu_armgl.o"
"${ARCH}ar" rcs "$BUNDLE_PREFIX/lib/libarmos-wlcomp-gpu.a" \
    "$WORK_DIR/wlcomp-gpu/wlcomp_gpu_armgl.o"
if ! "${ARCH}nm" -g --defined-only \
        "$BUNDLE_PREFIX/lib/libarmos-wlcomp-gpu.a" | \
        awk '$NF == "wl_gpu_backend_provider_create" { found = 1 } \
             END { exit !found }'; then
    echo "error: compositor GPU provider archive is invalid" >&2
    exit 1
fi

echo "=== Linking ArmOS EGL smoke test ==="
mkdir -p "$BUNDLE_ROOT/usr/bin" "$WORK_DIR/smoke"
"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -I"$BUNDLE_PREFIX/include" -I"$ROOT_DIR/userland/include" \
    -I"$ROOT_DIR/include" -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/egl-smoke/egl-smoke.c" \
    -o "$WORK_DIR/smoke/egl-smoke.o"
"${ARCH}gcc" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TARGET_TEXT_ADDRESS" -Wl,-e,_start \
    -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -o "$BUNDLE_ROOT/usr/bin/egl-smoke" \
    $RUNTIME_OBJECTS "$WORK_DIR/smoke/egl-smoke.o" \
    -Wl,--start-group \
    "$BUNDLE_PREFIX/lib/libEGL.a" "$BUNDLE_PREFIX/lib/libGLESv2.a" \
    "$ARMOS_WAYLAND_EGL_LIBRARY" "$ARMOS_WAYLAND_CLIENT_LIBRARY" \
    "$ARMOS_WINSYS_LIBRARY" "$ARMOS_SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" \
    "$(${ARCH}gcc $ARM_FLAGS -print-libgcc-file-name)" \
    -Wl,--end-group

echo "=== Linking ArmOS compositor GPU pipeline smoke test ==="
"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_backend.c" \
    -o "$WORK_DIR/smoke/gpu_backend.o"
"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/armos-wlcomp/gpu_present.c" \
    -o "$WORK_DIR/smoke/gpu_present.o"
"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/armgl-compositor-smoke/armgl-compositor-smoke.c" \
    -o "$WORK_DIR/smoke/armgl-compositor-smoke.o"
"${ARCH}gcc" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TARGET_TEXT_ADDRESS" -Wl,-e,_start \
    -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -Wl,-u,wl_gpu_backend_provider_create \
    -o "$BUNDLE_ROOT/usr/bin/armgl-compositor-smoke" \
    $RUNTIME_OBJECTS \
    "$WORK_DIR/smoke/gpu_backend.o" \
    "$WORK_DIR/smoke/gpu_present.o" \
    "$WORK_DIR/smoke/armgl-compositor-smoke.o" \
    -Wl,--whole-archive \
    "$BUNDLE_PREFIX/lib/libarmos-wlcomp-gpu.a" \
    -Wl,--no-whole-archive -Wl,--start-group \
    "$BUNDLE_PREFIX/lib/libEGL.a" "$BUNDLE_PREFIX/lib/libGLESv2.a" \
    "$ARMOS_WINSYS_LIBRARY" "$ARMOS_SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" \
    "$(${ARCH}gcc $ARM_FLAGS -print-libgcc-file-name)" \
    -Wl,--end-group

echo "=== Linking ArmOS external GPU image smoke test ==="
"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
    -I"$BUNDLE_PREFIX/include" -I"$ROOT_DIR/userland/include" \
    -I"$ROOT_DIR/include" -I"$NEWLIB_SYSROOT/include" \
    -I"$BUILD_DIR/src" -I"$BUILD_DIR/src/util/format" \
    -I"$SOURCE_DIR/include" -I"$SOURCE_DIR/src" \
    -I"$SOURCE_DIR/src/gallium/include" \
    -I"$SOURCE_DIR/src/gallium/frontends/armgl" \
    -I"$SOURCE_DIR/src/gallium/targets/armos" \
    -c "$ROOT_DIR/userland/programs/armgl-import-smoke/armgl-import-smoke.c" \
    -o "$WORK_DIR/smoke/armgl-import-smoke.o"
"${ARCH}gcc" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TARGET_TEXT_ADDRESS" -Wl,-e,_start \
    -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -o "$BUNDLE_ROOT/usr/bin/armgl-import-smoke" \
    $RUNTIME_OBJECTS "$WORK_DIR/smoke/armgl-import-smoke.o" \
    -Wl,--start-group \
    "$BUNDLE_PREFIX/lib/libEGL.a" "$BUNDLE_PREFIX/lib/libGLESv2.a" \
    "$ARMOS_WINSYS_LIBRARY" "$ARMOS_SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" \
    "$(${ARCH}gcc $ARM_FLAGS -print-libgcc-file-name)" \
    -Wl,--end-group

"${ARCH}gcc" $ARM_FLAGS -std=gnu99 -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -I"$BUNDLE_PREFIX/include" -I"$ROOT_DIR/userland/include" \
    -I"$ROOT_DIR/include" -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/egl-wayland-smoke/egl-wayland-smoke.c" \
    -o "$WORK_DIR/smoke/egl-wayland-smoke.o"
"${ARCH}gcc" $ARM_FLAGS -nostdlib -nostartfiles -static \
    -Wl,-Ttext="$TARGET_TEXT_ADDRESS" -Wl,-e,_start \
    -Wl,--gc-sections -Wl,--allow-multiple-definition \
    -o "$BUNDLE_ROOT/usr/bin/egl-wayland-smoke" \
    $RUNTIME_OBJECTS "$WORK_DIR/smoke/egl-wayland-smoke.o" \
    -Wl,--start-group \
    "$BUNDLE_PREFIX/lib/libEGL.a" "$BUNDLE_PREFIX/lib/libGLESv2.a" \
    "$ARMOS_WAYLAND_EGL_LIBRARY" "$ARMOS_WAYLAND_CLIENT_LIBRARY" \
    "$ARMOS_WINSYS_LIBRARY" "$ARMOS_SYSLOG_LIBRARY" \
    "$NEWLIB_LIBM" "$NEWLIB_LIBC" \
    "$(${ARCH}gcc $ARM_FLAGS -print-libgcc-file-name)" \
    -Wl,--end-group

if [ "$TARGET_ARCH" = "arm64" ]; then
    expected_machine="AArch64"
else
    expected_machine="ARM"
fi
if ! "${ARCH}readelf" -h "$BUNDLE_ROOT/usr/bin/egl-smoke" \
        | grep -q "Machine:.*$expected_machine"; then
    echo "error: EGL smoke test has an invalid target architecture" >&2
    exit 1
fi
if ! "${ARCH}readelf" -h "$BUNDLE_ROOT/usr/bin/egl-wayland-smoke" \
        | grep -q "Machine:.*$expected_machine"; then
    echo "error: EGL Wayland smoke test has an invalid target architecture" >&2
    exit 1
fi
if ! "${ARCH}readelf" -h "$BUNDLE_ROOT/usr/bin/armgl-import-smoke" \
        | grep -q "Machine:.*$expected_machine"; then
    echo "error: ArmGL import smoke test has an invalid target architecture" >&2
    exit 1
fi
if ! "${ARCH}readelf" -h "$BUNDLE_ROOT/usr/bin/armgl-compositor-smoke" \
        | grep -q "Machine:.*$expected_machine"; then
    echo "error: compositor GPU smoke test has an invalid target architecture" >&2
    exit 1
fi

echo
echo "ArmOS Mesa $MESA_VERSION bundle built for $TARGET_ARCH/$TARGET_PLATFORM:"
echo "  $BUNDLE_ROOT"
