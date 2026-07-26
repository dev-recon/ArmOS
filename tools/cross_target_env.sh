#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/cross_target_env.sh
# Layer: Host tooling / cross-build configuration
#
# Responsibilities:
# - Derive one coherent userland target contract from the compiler prefix.
# - Share CPU flags, newlib paths, runtime objects and ELF load addresses.
#
# Notes:
# - This file is sourced by build scripts after ROOT_DIR and ARCH are set.
# - Generated root filesystems and bundles are isolated by target.

ARMOS_REPRODUCIBLE_ROOT="${ARMOS_REPRODUCIBLE_ROOT:-/usr/src/armos}"
ARMOS_REPRO_FLAGS="\
-ffile-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT \
-fmacro-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT \
-fdebug-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT"
TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"

case "${ARCH:-}" in
    arm-none-eabi-)
        TARGET_ARCH="${TARGET_ARCH:-arm32}"
        TARGET_TRIPLET="arm-none-eabi"
        ARM_FLAGS="${ARM_FLAGS:--mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft}"
        TARGET_TEXT_ADDRESS="${TARGET_TEXT_ADDRESS:-0x8000}"
        ;;
    aarch64-elf-)
        TARGET_ARCH="${TARGET_ARCH:-arm64}"
        TARGET_TRIPLET="aarch64-elf"
        ARM_FLAGS="${ARM_FLAGS:--mcpu=cortex-a53}"
        TARGET_TEXT_ADDRESS="${TARGET_TEXT_ADDRESS:-0x100000000}"
        ;;
    *)
        echo "error: unsupported ArmOS cross compiler prefix: ${ARCH:-<unset>}" >&2
        return 2 2>/dev/null || exit 2
        ;;
esac

case "$ARCH:$TARGET_ARCH" in
    arm-none-eabi-:arm32|aarch64-elf-:arm64)
        ;;
    *)
        echo "error: cross compiler '$ARCH' does not match TARGET_ARCH='$TARGET_ARCH'" >&2
        return 2 2>/dev/null || exit 2
        ;;
esac

case " $ARM_FLAGS " in
    *" -ffile-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT "*)
        ;;
    *)
        ARM_FLAGS="$ARM_FLAGS $ARMOS_REPRO_FLAGS"
        ;;
esac
TARGET_BUILD_ROOT="${TARGET_BUILD_ROOT:-$ROOT_DIR/build/$TARGET_ARCH/$TARGET_PLATFORM}"
NEWLIB_RUNTIME_DIR="${NEWLIB_RUNTIME_DIR:-$TARGET_BUILD_ROOT/newlib-port}"
NEWLIB_SYSROOT="${NEWLIB_SYSROOT:-$TARGET_BUILD_ROOT/newlib-sysroot/$TARGET_TRIPLET}"
NEWLIB_LIBC="${NEWLIB_LIBC:-$NEWLIB_SYSROOT/lib/libc.a}"
NEWLIB_LIBM="${NEWLIB_LIBM:-$NEWLIB_SYSROOT/lib/libm.a}"
RUNTIME_OBJECTS="${RUNTIME_OBJECTS:-$NEWLIB_RUNTIME_DIR/crt0_newlib.o $NEWLIB_RUNTIME_DIR/syscall_raw.o $NEWLIB_RUNTIME_DIR/syscalls.o $NEWLIB_RUNTIME_DIR/stdio_lock.o $NEWLIB_RUNTIME_DIR/pthread.o $NEWLIB_RUNTIME_DIR/pthread_sync.o}"
USERFS_ROOT="${USERFS_ROOT:-$TARGET_BUILD_ROOT/userfs}"
BUNDLE_BUILD_ROOT="${BUNDLE_BUILD_ROOT:-$TARGET_BUILD_ROOT/bundles}"

export TARGET_ARCH TARGET_TRIPLET ARM_FLAGS TARGET_TEXT_ADDRESS
export ARMOS_REPRODUCIBLE_ROOT ARMOS_REPRO_FLAGS
export NEWLIB_SYSROOT NEWLIB_LIBC NEWLIB_LIBM NEWLIB_RUNTIME_DIR RUNTIME_OBJECTS
export TARGET_PLATFORM TARGET_BUILD_ROOT USERFS_ROOT BUNDLE_BUILD_ROOT
