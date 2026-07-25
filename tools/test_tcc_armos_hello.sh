#!/usr/bin/env bash
# test_tcc_armos_hello.sh - validate the first ArmOS TinyCC compilation path.
#
# Current milestone:
#   1. Use host-built TinyCC `arm-eabi-tcc` to compile ARM EABI object code.
#   2. Prove TinyCC can link a tiny static ArmOS executable without newlib.
#   3. Prove TinyCC-generated objects still link through the stable ArmOS
#      GCC/newlib path.
#   4. Prove TinyCC can link a complete newlib-backed ArmOS binary when it is
#      given the ARM/EABI root libgcc runtime instead of GCC's Thumb multilib.
#
# This intentionally does not claim full native TCC support yet. It validates
# the host-side cross path and the linker/runtime recipe that the future native
# /opt/tcc/bin/tcc wrapper must use inside ArmOS.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TCC_BUILD="${TCC_BUILD:-$ROOT_DIR/build/tcc-host-arm}"
TCC="$TCC_BUILD/arm-eabi-tcc"
OUT_DIR="${OUT_DIR:-/private/tmp}"
MIN_OBJ="$OUT_DIR/armos-tcc-minimal_exit.o"
MIN_BIN="$OUT_DIR/armos-tcc-minimal_exit"
MIN_SYSCALLS="$OUT_DIR/armos-tcc-syscalls_min.o"
OBJ="$OUT_DIR/armos-tcc-hello.o"
BIN="$OUT_DIR/armos-tcc-hello-gccld"
TCC_NEWLIB_BIN="$OUT_DIR/armos-tcc-hello-tccld"
ARCH="${ARCH:-arm-none-eabi-}"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

if [ ! -x "$TCC" ]; then
    echo "error: $TCC not found" >&2
    echo "hint: run ./tools/build_tcc_host.sh first" >&2
    exit 1
fi

STABLE_LIBGCC="$(${ARCH}gcc -mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 \
    -mfloat-abi=soft -print-libgcc-file-name)"
TCC_LIBGCC="$(${ARCH}gcc -print-libgcc-file-name)"

echo "== TinyCC minimal static ArmOS link =="
"$TCC" \
    -c "$ROOT_DIR/userland/opt/tcc/tests/minimal_exit.c" \
    -o "$MIN_OBJ"

"$TCC" \
    -static -nostdlib -Wl,-Ttext=0x8000 -Wl,-e,_start \
    -o "$MIN_BIN" \
    "$MIN_OBJ" \
    "$ROOT_DIR/newlib-port/build/syscall_raw.o"

file "$MIN_OBJ"
file "$MIN_BIN"
"${ARCH}readelf" -h "$MIN_BIN" | sed -n '1,18p'
"${ARCH}readelf" -l "$MIN_BIN" | sed -n '1,22p'

echo
echo "== TinyCC compile + stable GCC/newlib link =="
"$TCC" \
    -c "$ROOT_DIR/userland/programs/hello/main.c" \
    -o "$OBJ" \
    -I"$ROOT_DIR/userland/include" \
    -I"$ROOT_DIR/build/newlib-sysroot/arm-none-eabi/include" \
    -I"$ROOT_DIR/userland/opt/tcc/src/include" \
    -DARM_OS_NEWLIB

"${ARCH}gcc" \
    -mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft \
    -nostdlib -nostartfiles -static \
    -Wl,-Ttext=0x8000 -Wl,-e,_start -Wl,--gc-sections \
    -Wl,--allow-multiple-definition \
    -o "$BIN" \
    "$ROOT_DIR/newlib-port/build/crt0_newlib.o" \
    "$ROOT_DIR/newlib-port/build/syscall_raw.o" \
    "$ROOT_DIR/newlib-port/build/syscalls.o" \
    "$ROOT_DIR/newlib-port/build/stdio_lock.o" \
    "$OBJ" \
    "$ROOT_DIR/build/newlib-sysroot/arm-none-eabi/lib/libc.a" \
    "$STABLE_LIBGCC"

file "$OBJ"
file "$BIN"
"${ARCH}objdump" -f "$BIN"

echo
echo "== TinyCC + newlib link with root ARM/EABI libgcc =="
"${ARCH}gcc" \
    -mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft \
    -std=gnu99 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -ffunction-sections -fdata-sections -Os \
    -I"$ROOT_DIR/include" \
    -I"$ROOT_DIR/build/newlib-sysroot/arm-none-eabi/include" \
    -c "$ROOT_DIR/newlib-port/tcc/syscalls_min.c" \
    -o "$MIN_SYSCALLS"

"$TCC" \
    -static -nostdlib -Wl,-Ttext=0x8000 -Wl,-e,_start \
    -o "$TCC_NEWLIB_BIN" \
    "$ROOT_DIR/newlib-port/build/crt0_newlib.o" \
    "$ROOT_DIR/newlib-port/build/syscall_raw.o" \
    "$MIN_SYSCALLS" \
    "$ROOT_DIR/newlib-port/build/stdio_lock.o" \
    "$OBJ" \
    "$ROOT_DIR/build/newlib-sysroot/arm-none-eabi/lib/libc.a" \
    "$TCC_LIBGCC"

file "$TCC_NEWLIB_BIN"
"${ARCH}objdump" -f "$TCC_NEWLIB_BIN"
"${ARCH}readelf" -h "$TCC_NEWLIB_BIN" | sed -n '1,18p'

echo
echo "TinyCC ArmOS host-side validation passed."
echo "stable GCC/newlib libgcc: $STABLE_LIBGCC"
echo "TinyCC linker libgcc:     $TCC_LIBGCC"
