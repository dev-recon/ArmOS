#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/check_mesa_virgl_adapter.sh
# Layer: Host tooling / Mesa port validation
#
# Responsibilities:
# - Compile the ArmOS VirGL winsys against the selected upstream Mesa headers.
# - Optionally compile the ArmOS Gallium and EGL frontends against a build.
# - Generate required Mesa format headers in an isolated temporary directory.
# - Validate the adapter with the real ARM32 or ARM64 target compiler.
#
# Notes:
# - This is a contract check, not the final Mesa/EGL bundle build.
# - It never writes target objects into the repository build directories.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MESA_SOURCE_DIR="${MESA_SOURCE_DIR:-}"
MESA_PYTHON="${MESA_PYTHON:-python3}"
MESA_BUILD_DIR="${MESA_BUILD_DIR:-}"
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

if [ -z "$MESA_SOURCE_DIR" ] ||
   [ ! -f "$MESA_SOURCE_DIR/src/gallium/drivers/virgl/virgl_winsys.h" ]; then
    echo "error: set MESA_SOURCE_DIR to a complete Mesa 25.3.6 source tree" >&2
    exit 2
fi

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

if ! "$MESA_PYTHON" -c 'import yaml' >/dev/null 2>&1; then
    echo "error: $MESA_PYTHON cannot import PyYAML required by Mesa generators" >&2
    echo "set MESA_PYTHON to the pinned Mesa host-tools interpreter" >&2
    exit 2
fi

CHECK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/armos-mesa-adapter.XXXXXX")"
trap 'rm -rf "$CHECK_ROOT"' EXIT HUP INT TERM
mkdir -p "$CHECK_ROOT/generated/util/format"

PYTHONPATH="$MESA_SOURCE_DIR/src/util/format${PYTHONPATH:+:$PYTHONPATH}" \
    "$MESA_PYTHON" \
    "$MESA_SOURCE_DIR/src/util/format/u_format_table.py" \
    "$MESA_SOURCE_DIR/src/util/format/u_format.yaml" --enums \
    > "$CHECK_ROOT/generated/util/format/u_format_gen.h"

EXTRA_FLAGS=""
if [ "$TARGET_ARCH" = "arm32" ]; then
    # Mesa requires four-byte Gallium enums; bare-metal GCC otherwise selects
    # its short-enum ABI and rejects wide enum bitfields in pipe_state.
    EXTRA_FLAGS="-fno-short-enums"
fi

"${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=gnu11 -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector \
    -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ \
    -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$CHECK_ROOT/generated" \
    -I"$ROOT_DIR/userland/opt/mesa/armos" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
    -I"$MESA_SOURCE_DIR/src/gallium/include" \
    -I"$MESA_SOURCE_DIR/src/gallium/auxiliary" \
    -I"$MESA_SOURCE_DIR/src/gallium/drivers" \
    -I"$MESA_SOURCE_DIR/src/virtio" \
    -c "$ROOT_DIR/userland/opt/mesa/armos/virgl_armos_winsys.c" \
    -o "$CHECK_ROOT/virgl_armos_winsys.o"

"${ARCH}readelf" -h "$CHECK_ROOT/virgl_armos_winsys.o" >/dev/null

"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector \
    -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/lib/wayland/egl.c" \
    -o "$CHECK_ROOT/wayland_egl.o"
"${ARCH}readelf" -h "$CHECK_ROOT/wayland_egl.o" >/dev/null

for source in gpu_backend gpu_present; do
    "${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -Wall -Wextra -Werror \
        -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -c "$ROOT_DIR/userland/programs/armos-wlcomp/$source.c" \
        -o "$CHECK_ROOT/$source.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/$source.o" >/dev/null
done

"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector \
    -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/userland/programs/armgl-compositor-smoke/armgl-compositor-smoke.c" \
    -o "$CHECK_ROOT/armgl_compositor_smoke.o"
"${ARCH}readelf" -h "$CHECK_ROOT/armgl_compositor_smoke.o" >/dev/null

"${ARCH}gcc" $ARM_FLAGS -std=gnu11 -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector \
    -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
    -Wall -Wextra -Werror \
    -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" -I"$MESA_SOURCE_DIR/include" \
    -c "$ROOT_DIR/userland/programs/egl-wayland-smoke/egl-wayland-smoke.c" \
    -o "$CHECK_ROOT/egl_wayland_smoke.o"
"${ARCH}readelf" -h "$CHECK_ROOT/egl_wayland_smoke.o" >/dev/null

if [ -n "$MESA_BUILD_DIR" ]; then
    if [ ! -f "$MESA_BUILD_DIR/src/util/format/u_format_gen.h" ]; then
        echo "error: MESA_BUILD_DIR is not a configured Mesa build: $MESA_BUILD_DIR" >&2
        exit 2
    fi

    # State-tracker headers deliberately expose more Mesa internals than the
    # winsys.  Keep diagnostics on our API boundary without turning warnings
    # in upstream inline helpers into ArmOS build failures.
    "${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=c11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
        -DHAVE_STRUCT_TIMESPEC -DHAVE_PTHREAD \
        -DHAVE_OPENGL_ES_2=1 -DHAVE_VIRGL \
        -D_POSIX_C_SOURCE=200809L \
        -Wall -Werror=implicit-function-declaration \
        -Werror=incompatible-pointer-types -Werror=int-conversion \
        -I"$MESA_BUILD_DIR/src" -I"$MESA_BUILD_DIR/src/mesa" \
        -I"$MESA_BUILD_DIR/src/util/format" \
        -I"$ROOT_DIR/userland/opt/mesa/armos" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
        -I"$MESA_SOURCE_DIR/src/gallium/include" \
        -I"$MESA_SOURCE_DIR/src/gallium/auxiliary" \
        -I"$MESA_SOURCE_DIR/src/mesa" \
        -c "$ROOT_DIR/userland/opt/mesa/armos/armgl_frontend.c" \
        -o "$CHECK_ROOT/armgl_frontend.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/armgl_frontend.o" >/dev/null

    "${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=gnu11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
        -Wall -Wextra -Werror \
        -I"$MESA_BUILD_DIR/src" -I"$MESA_BUILD_DIR/src/util/format" \
        -I"$ROOT_DIR/userland/opt/mesa/armos" \
        -I"$ROOT_DIR/userland/programs/armos-wlcomp" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
        -I"$MESA_SOURCE_DIR/src/gallium/include" \
        -c "$ROOT_DIR/userland/opt/mesa/armos/wlcomp_gpu_armgl.c" \
        -o "$CHECK_ROOT/wlcomp_gpu_armgl.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/wlcomp_gpu_armgl.o" >/dev/null

    "${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=gnu11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
        -Wall -Wextra -Werror \
        -I"$MESA_BUILD_DIR/src" -I"$MESA_BUILD_DIR/src/util/format" \
        -I"$ROOT_DIR/userland/opt/mesa/armos" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
        -I"$MESA_SOURCE_DIR/src/gallium/include" \
        -c "$ROOT_DIR/userland/programs/armgl-import-smoke/armgl-import-smoke.c" \
        -o "$CHECK_ROOT/armgl_import_smoke.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/armgl_import_smoke.o" >/dev/null

    "${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=c11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -D_POSIX_C_SOURCE=200809L \
        -Wall -Werror=implicit-function-declaration \
        -Werror=incompatible-pointer-types -Werror=int-conversion \
        -I"$ROOT_DIR/userland/opt/mesa/armos" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
        -I"$MESA_SOURCE_DIR/src/gallium/include" \
        -c "$ROOT_DIR/userland/opt/mesa/armos/pipe_loader_armos.c" \
        -o "$CHECK_ROOT/pipe_loader_armos.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/pipe_loader_armos.o" >/dev/null

    "${ARCH}gcc" $ARM_FLAGS $EXTRA_FLAGS -std=c11 -O2 \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -DARM_OS_NEWLIB -D__DYNAMIC_REENT__ -D__ARMOS__ \
        -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
        -DHAVE_STRUCT_TIMESPEC -DHAVE_PTHREAD \
        -DHAVE_OPENGL_ES_2=1 -DHAVE_VIRGL \
        -D_POSIX_C_SOURCE=200809L \
        -Wall -Werror=implicit-function-declaration \
        -Werror=incompatible-pointer-types -Werror=int-conversion \
        -I"$MESA_BUILD_DIR/src" -I"$MESA_BUILD_DIR/src/mesa" \
        -I"$MESA_BUILD_DIR/src/util/format" \
        -I"$ROOT_DIR/userland/opt/mesa/armos" \
        -I"$ROOT_DIR/userland/include" -I"$ROOT_DIR/include" \
        -I"$NEWLIB_SYSROOT/include" \
        -I"$MESA_SOURCE_DIR/include" -I"$MESA_SOURCE_DIR/src" \
        -I"$MESA_SOURCE_DIR/src/egl/main" \
        -I"$MESA_SOURCE_DIR/src/gallium/include" \
        -I"$MESA_SOURCE_DIR/src/gallium/auxiliary" \
        -I"$MESA_SOURCE_DIR/src/mesa" \
        -c "$ROOT_DIR/userland/opt/mesa/armos/egl_armos.c" \
        -o "$CHECK_ROOT/egl_armos.o"
    "${ARCH}readelf" -h "$CHECK_ROOT/egl_armos.o" >/dev/null
fi

echo "Mesa VirGL adapter contract valid for $TARGET_ARCH/$TARGET_PLATFORM"
