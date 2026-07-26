#!/usr/bin/env bash
# build_tcc_host.sh - build the first ArmOS TinyCC host-side cross compiler.
#
# This is an early porting helper, not part of the mandatory ArmOS build. It
# builds TinyCC's upstream ARM cross target out-of-tree so we can validate ARM
# code generation and the ArmOS linker recipe before producing a native
# /opt/tcc/bin/tcc binary.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT_DIR/userland/opt/tcc/src"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/arm32/qemu-virt/tools/tcc-host}"
CC_HOST="${CC_HOST:-clang}"

if [ ! -f "$SRC_DIR/configure" ]; then
    echo "error: TinyCC sources not found in $SRC_DIR" >&2
    echo "hint: import upstream TinyCC before running this helper" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$SRC_DIR/configure" \
    --source-path="$SRC_DIR" \
    --prefix=/opt/tcc \
    --cc="$CC_HOST" \
    --extra-cflags=-O2 \
    --enable-static

make cross-arm cross-arm-eabi

cat <<EOF

TinyCC ARM host toolchain built:
  $BUILD_DIR/arm-tcc
  $BUILD_DIR/arm-libtcc1.a
  $BUILD_DIR/arm-eabi-tcc
  $BUILD_DIR/arm-eabi-libtcc1.a

The arm-eabi target is the preferred starting point for ArmOS experiments.
Full ArmOS/newlib linking works when TinyCC is given GCC's root ARM/EABI
libgcc.a instead of the Thumb multilib selected by the normal userland flags.
Use
  ./tools/test_tcc_armos_hello.sh
to validate the current bring-up boundary.
EOF
