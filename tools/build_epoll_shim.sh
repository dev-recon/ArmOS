#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_epoll_shim.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Cross-build the ArmOS epoll compatibility library.
# - Package its public header, static archive and regression test.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${SRC_DIR:-$ROOT_DIR/userland/opt/epoll-shim/src}"
TEST_SRC="$ROOT_DIR/userland/opt/epoll-shim/test/epoll-test.c"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/epoll-shim}"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/epoll-shim"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"

LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

if [ ! -f "$SRC_DIR/epoll.c" ] ||
   [ ! -f "$SRC_DIR/sys/epoll.h" ] ||
   [ ! -f "$SRC_DIR/epoll-shim.pc" ] ||
   [ ! -f "$TEST_SRC" ]; then
    echo "error: ArmOS epoll compatibility sources are incomplete" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    echo "hint: run ./tools/build_newlib.sh first" >&2
    exit 1
fi

if [ ! -f "$NEWLIB_RUNTIME_DIR/crt0_newlib.o" ] ||
   [ ! -f "$NEWLIB_RUNTIME_DIR/syscall_raw.o" ] ||
   [ ! -f "$NEWLIB_RUNTIME_DIR/syscalls.o" ]; then
    echo "error: newlib-port runtime objects are missing" >&2
    echo "hint: make -C newlib-port NEWLIB_SYSROOT=$NEWLIB_SYSROOT" >&2
    exit 1
fi

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/sys" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN"

CFLAGS="$ARM_FLAGS -std=gnu99 -Os -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -I$SRC_DIR -I$ROOT_DIR/userland/include -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections -Wl,--allow-multiple-definition"

"$CC" $CFLAGS -c "$SRC_DIR/epoll.c" -o "$BUILD_DIR/epoll.o"
"$AR" rcs "$BUNDLE_PREFIX/lib/libepoll-shim.a" "$BUILD_DIR/epoll.o"
"$RANLIB" "$BUNDLE_PREFIX/lib/libepoll-shim.a"
cp "$SRC_DIR/sys/epoll.h" "$BUNDLE_PREFIX/include/sys/epoll.h"
cp "$SRC_DIR/epoll-shim.pc" "$BUNDLE_PREFIX/lib/pkgconfig/epoll-shim.pc"

"$CC" $CFLAGS -c "$TEST_SRC" -o "$BUILD_DIR/epoll-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/epoll-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/epoll-test.o" \
    "$BUNDLE_PREFIX/lib/libepoll-shim.a" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"

"$STRIP" --strip-all "$BUNDLE_USR_BIN/epoll-test" || true

echo
echo "ArmOS epoll compatibility bundle built:"
echo "  $BUNDLE_ROOT"
