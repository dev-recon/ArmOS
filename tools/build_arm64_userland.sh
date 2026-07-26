#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_arm64_userland.sh
# Layer: Host tooling / ARM64 userland
#
# Responsibilities:
# - Build the complete generic ArmOS userland with the AArch64 newlib port.
# - Optionally install ELF64 programs into the target userfs hierarchy.
# - Validate every generated and installed executable as AArch64 ELF64.
#
# Notes:
# - ARM32 and ARM64 intentionally use the same userland target and path sets.
# - Generated files remain below build/arm64/<platform>.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"
TARGET_BUILD_ROOT="$ROOT_DIR/build/arm64/$TARGET_PLATFORM"

if [ "${ARMOS_BUILD_LOCK_HELD:-0}" != "1" ]; then
    export ARMOS_BUILD_LOCK_DIR="$TARGET_BUILD_ROOT/.build.lock"
    exec "$ROOT_DIR/tools/with_build_lock.sh" "$0" "$@"
fi

SYSROOT="${NEWLIB_SYSROOT:-$TARGET_BUILD_ROOT/newlib-sysroot/aarch64-elf}"
LIBC="$SYSROOT/lib/libc.a"
TARGETS=()
REBUILD_NEWLIB=0
CLEAN=0
INSTALL=0

usage()
{
    cat <<'EOF'
usage: tools/build_arm64_userland.sh [OPTIONS] [target ...]

Builds the AArch64 ArmOS newlib glue and userland. With no target, builds the
complete generic userland target set.

  --clean           remove previous ARM64 userland output first
  --install         build all targets and install them into target userfs
  --rebuild-newlib  rebuild the repo-local AArch64 newlib sysroot
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rebuild-newlib)
            REBUILD_NEWLIB=1
            ;;
        --clean)
            CLEAN=1
            ;;
        --install)
            INSTALL=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            TARGETS+=("$1")
            ;;
    esac
    shift
done

if [ "$INSTALL" -eq 1 ] && [ "${#TARGETS[@]}" -ne 0 ]; then
    echo "error: --install always installs the complete userland" >&2
    exit 2
fi
if [ "$INSTALL" -eq 1 ]; then
    TARGETS=(install)
elif [ "${#TARGETS[@]}" -eq 0 ]; then
    TARGETS=(all)
fi

if [ "$REBUILD_NEWLIB" -eq 1 ] ||
   [ ! -f "$SYSROOT/include/stdio.h" ] || [ ! -f "$LIBC" ] ||
   [ ! -f "$SYSROOT/.armos-reproducible-paths-v1" ]; then
    TARGET=aarch64-elf ARCH=aarch64-elf- \
        NEWLIB_BUILD_ROOT="$TARGET_BUILD_ROOT/newlib-build" \
        NEWLIB_INSTALL_ROOT="$TARGET_BUILD_ROOT/newlib-sysroot" \
        "$ROOT_DIR/tools/build_newlib.sh"
fi

make -C "$ROOT_DIR/newlib-port" \
    TARGET_ARCH=arm64 \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    BUILD_DIR="$TARGET_BUILD_ROOT/newlib-port" \
    ARCH=aarch64-elf- \
    NEWLIB_SYSROOT="$SYSROOT" \
    NEWLIB_LIBC="$LIBC"

if [ "$CLEAN" -eq 1 ]; then
    make -C "$ROOT_DIR/userland" \
        TARGET_ARCH=arm64 \
        TARGET_PLATFORM="$TARGET_PLATFORM" \
        TARGET_BUILD_ROOT="$TARGET_BUILD_ROOT" \
        ARCH=aarch64-elf- \
        NEWLIB_SYSROOT="$SYSROOT" \
        NEWLIB_LIBC="$LIBC" \
        clean
fi

if [ "$INSTALL" -eq 1 ]; then
    TARGET_ARCH=arm64 TARGET_PLATFORM="$TARGET_PLATFORM" \
        USERFS_ROOT="$TARGET_BUILD_ROOT/userfs" \
        "$ROOT_DIR/tools/prepare_target_userfs.sh"
    rm -f "$TARGET_BUILD_ROOT/userland/out/usr/bin/hello64"
fi

make -C "$ROOT_DIR/userland" \
    TARGET_ARCH=arm64 \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    TARGET_BUILD_ROOT="$TARGET_BUILD_ROOT" \
    USERFS_ROOT="$TARGET_BUILD_ROOT/userfs" \
    NEWLIB_RUNTIME_DIR="$TARGET_BUILD_ROOT/newlib-port" \
    ARCH=aarch64-elf- \
    NEWLIB_SYSROOT="$SYSROOT" \
    NEWLIB_LIBC="$LIBC" \
    "${TARGETS[@]}"

OUT_DIR="$TARGET_BUILD_ROOT/userland/out"
USERFS_DIR="$TARGET_BUILD_ROOT/userfs"
EXECUTABLES=0
while IFS= read -r binary; do
    if ! aarch64-elf-readelf -h "$binary" | grep -q 'Class:.*ELF64' ||
       ! aarch64-elf-readelf -h "$binary" | grep -q 'Machine:.*AArch64'; then
        echo "error: expected AArch64 ELF64 output was not produced: $binary" >&2
        exit 1
    fi
    EXECUTABLES=$((EXECUTABLES + 1))
done < <(find "$OUT_DIR" -type f | sort)

if [ "$EXECUTABLES" -eq 0 ]; then
    echo "error: no AArch64 userland executable was produced" >&2
    exit 1
fi

if [ "$INSTALL" -eq 1 ]; then
    while IFS= read -r binary; do
        relative="${binary#"$OUT_DIR"/}"
        installed="$USERFS_DIR/$relative"
        if [ ! -f "$installed" ] ||
           ! aarch64-elf-readelf -h "$installed" | grep -q 'Class:.*ELF64' ||
           ! aarch64-elf-readelf -h "$installed" | grep -q 'Machine:.*AArch64'; then
            echo "error: invalid installed AArch64 executable: $installed" >&2
            exit 1
        fi
    done < <(find "$OUT_DIR" -type f | sort)
    echo "AArch64 userland installed in target userfs: $EXECUTABLES executables"
else
    echo "AArch64 userland ready: $EXECUTABLES executables"
fi
