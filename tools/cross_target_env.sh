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
# - The shared userfs contains one active architecture at a time.

ARMOS_REPRODUCIBLE_ROOT="${ARMOS_REPRODUCIBLE_ROOT:-/usr/src/armos}"
ARMOS_REPRO_FLAGS="\
-ffile-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT \
-fmacro-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT \
-fdebug-prefix-map=$ROOT_DIR=$ARMOS_REPRODUCIBLE_ROOT"

case "${ARCH:-}" in
    arm-none-eabi-)
        TARGET_ARCH="${TARGET_ARCH:-arm32}"
        TARGET_TRIPLET="arm-none-eabi"
        ARM_FLAGS="${ARM_FLAGS:--mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft}"
        TARGET_TEXT_ADDRESS="${TARGET_TEXT_ADDRESS:-0x8000}"
        NEWLIB_RUNTIME_DIR="${NEWLIB_RUNTIME_DIR:-$ROOT_DIR/newlib-port/build}"
        ;;
    aarch64-elf-)
        TARGET_ARCH="${TARGET_ARCH:-arm64}"
        TARGET_TRIPLET="aarch64-elf"
        ARM_FLAGS="${ARM_FLAGS:--mcpu=cortex-a53}"
        TARGET_TEXT_ADDRESS="${TARGET_TEXT_ADDRESS:-0x100000000}"
        NEWLIB_RUNTIME_DIR="${NEWLIB_RUNTIME_DIR:-$ROOT_DIR/newlib-port/build/arm64}"
        ;;
    *)
        echo "error: unsupported ArmOS cross compiler prefix: ${ARCH:-<unset>}" >&2
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
NEWLIB_SYSROOT="${NEWLIB_SYSROOT:-$ROOT_DIR/build/newlib-sysroot/$TARGET_TRIPLET}"
NEWLIB_LIBC="${NEWLIB_LIBC:-$NEWLIB_SYSROOT/lib/libc.a}"
NEWLIB_LIBM="${NEWLIB_LIBM:-$NEWLIB_SYSROOT/lib/libm.a}"
RUNTIME_OBJECTS="${RUNTIME_OBJECTS:-$NEWLIB_RUNTIME_DIR/crt0_newlib.o $NEWLIB_RUNTIME_DIR/syscall_raw.o $NEWLIB_RUNTIME_DIR/syscalls.o $NEWLIB_RUNTIME_DIR/stdio_lock.o $NEWLIB_RUNTIME_DIR/pthread.o $NEWLIB_RUNTIME_DIR/pthread_sync.o}"

export TARGET_ARCH TARGET_TRIPLET ARM_FLAGS TARGET_TEXT_ADDRESS
export ARMOS_REPRODUCIBLE_ROOT ARMOS_REPRO_FLAGS
export NEWLIB_SYSROOT NEWLIB_LIBC NEWLIB_LIBM NEWLIB_RUNTIME_DIR RUNTIME_OBJECTS
