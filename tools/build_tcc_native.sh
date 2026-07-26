#!/usr/bin/env bash
# build_tcc_native.sh - build a native ArmOS TinyCC bundle.
#
# This helper intentionally builds out of the vendored upstream tree.  The
# native compiler is a normal ArmOS/newlib executable, while the programs it
# later produces use the smaller newlib-port/tcc runtime glue to avoid pulling
# duplicate high-level POSIX wrappers into TinyCC's linker.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT_DIR/userland/opt/tcc/src"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/bundles/tcc-native}"
PATCHED_SRC="$WORK_DIR/src"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_DIR="$WORK_DIR/bundle/opt/tcc"
ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"
CC="${ARCH}gcc"
AR="${ARCH}ar"
HOST_CC="${HOST_CC:-cc}"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# TinyCC crashes when linking ArmOS/newlib binaries with GCC's Thumb multilib
# libgcc.  The root ARM/EABI archive is the compatible runtime for this port.
TCC_LIBGCC="${TCC_LIBGCC:-$("$CC" -print-libgcc-file-name)}"
NATIVE_LIBGCC="${NATIVE_LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"

if [ "$TARGET_ARCH" = "arm64" ]; then
    TCC_CPU=arm64
    TCC_MAKE_TARGET=arm64-tcc
    TCC_OUTPUT=arm64-tcc
else
    TCC_CPU=arm
    TCC_MAKE_TARGET=arm-eabi-tcc
    TCC_OUTPUT=arm-eabi-tcc
fi

for required_source in configure conftest.c; do
    if [ ! -f "$SRC_DIR/$required_source" ]; then
        echo "error: TinyCC source '$required_source' not found in $SRC_DIR" >&2
        exit 1
    fi
done

if [ ! -f "$NEWLIB_SYSROOT/include/stdio.h" ] || [ ! -f "$NEWLIB_LIBC" ]; then
    echo "error: newlib sysroot is incomplete: $NEWLIB_SYSROOT" >&2
    echo "hint: run ./tools/build_newlib.sh and ./build.sh first" >&2
    exit 1
fi

rm -rf "$WORK_DIR"
mkdir -p "$PATCHED_SRC" "$BUILD_DIR/armos-include/sys" "$BUNDLE_DIR/bin" \
         "$BUNDLE_DIR/lib/tcc/include" "$BUNDLE_DIR/include/armos"

cp -R "$SRC_DIR"/. "$PATCHED_SRC"/

# ArmOS/newlib typedefs uint32_t as unsigned long on this target.  Upstream TCC
# declares o() as unsigned int in tcc.h, so keep the ARM backend signature
# exactly aligned with the public prototype.
perl -0pi -e 's/void o\(uint32_t i\)/void o(unsigned int i)/' "$PATCHED_SRC/arm-gen.c"

# The first native milestone does not support tcc -run.  A small mman header is
# still needed because tccrun.c is part of libtcc even when runtime execution is
# effectively disabled by the ArmOS build profile.
cat > "$BUILD_DIR/armos-include/sys/mman.h" <<'EOF'
#ifndef ARMOS_PORT_SYS_MMAN_H
#define ARMOS_PORT_SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);

#endif
EOF

cat > "$BUILD_DIR/armos_tcc_compat.c" <<'EOF'
#include <unistd.h>

long sysconf(int name)
{
    (void)name;
    return 4096;
}
EOF

"$CC" $ARM_FLAGS -std=gnu99 -Os -ffreestanding -fno-builtin \
    -fno-stack-protector -I"$NEWLIB_SYSROOT/include" \
    -c "$BUILD_DIR/armos_tcc_compat.c" \
    -o "$BUILD_DIR/armos_tcc_compat.o"

cd "$BUILD_DIR"

"$PATCHED_SRC/configure" \
    --source-path="$PATCHED_SRC" \
    --prefix=/opt/tcc \
    --cross-prefix="$ARCH" \
    --cc="gcc $ARM_FLAGS -ffreestanding -fno-builtin -fno-stack-protector -DARM_OS_NEWLIB -I$NEWLIB_SYSROOT/include" \
    --ar=ar \
    --cpu="$TCC_CPU" \
    --targetos=Linux \
    --triplet="$TARGET_TRIPLET" \
    --enable-static \
    --disable-rpath \
    --sysincludepaths=/opt/tcc/lib/tcc/include:/opt/tcc/include/armos:/opt/tcc/include \
    --libpaths=/opt/tcc/lib \
    --crtprefix=/opt/tcc/lib

# TCC's c2str generator is a build-host utility.  Keep it as a host executable
# even though the compiler being produced is an ArmOS executable.
"$HOST_CC" -DC2STR "$PATCHED_SRC/conftest.c" -o "$BUILD_DIR/c2str.exe"

TCC_CFLAGS="-Wall -O2 -Wdeclaration-after-statement -Wno-unused-result"
TCC_CFLAGS="$TCC_CFLAGS -DCONFIG_TCC_STATIC -DCONFIG_TCC_SEMLOCK=0 -DCONFIG_TCC_BACKTRACE=0"
TCC_CFLAGS="$TCC_CFLAGS -I$BUILD_DIR/armos-include"

NATIVE_LDFLAGS="-nostdlib -nostartfiles -static"
NATIVE_LDFLAGS="$NATIVE_LDFLAGS -Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections"
NATIVE_LDFLAGS="$NATIVE_LDFLAGS -Wl,--allow-multiple-definition"
NATIVE_LDFLAGS="$NATIVE_LDFLAGS $RUNTIME_OBJECTS"
NATIVE_LDFLAGS="$NATIVE_LDFLAGS $BUILD_DIR/armos_tcc_compat.o"
NATIVE_LDFLAGS="$NATIVE_LDFLAGS $NEWLIB_LIBM $NEWLIB_LIBC $NATIVE_LIBGCC"

make "$TCC_MAKE_TARGET" LIBS='' CFLAGS="$TCC_CFLAGS" LDFLAGS="$NATIVE_LDFLAGS"

"$CC" $ARM_FLAGS -std=gnu99 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -ffunction-sections -fdata-sections -Os \
    -I"$ROOT_DIR/userland/include" \
    -I"$ROOT_DIR/include" \
    -I"$NEWLIB_SYSROOT/include" \
    -c "$ROOT_DIR/newlib-port/tcc/syscalls_min.c" \
    -o "$BUNDLE_DIR/lib/syscalls_min.o"

cp "$BUILD_DIR/$TCC_OUTPUT" "$BUNDLE_DIR/bin/tcc"
cp "$NEWLIB_RUNTIME_DIR/crt0_newlib.o" "$BUNDLE_DIR/lib/crt0_newlib.o"
cp "$NEWLIB_RUNTIME_DIR/syscall_raw.o" "$BUNDLE_DIR/lib/syscall_raw.o"
cp "$NEWLIB_RUNTIME_DIR/stdio_lock.o" "$BUNDLE_DIR/lib/stdio_lock.o"
cp "$NEWLIB_LIBC" "$BUNDLE_DIR/lib/libc.a"
cp "$NEWLIB_LIBM" "$BUNDLE_DIR/lib/libm.a"
cp "$TCC_LIBGCC" "$BUNDLE_DIR/lib/libgcc.a"
if [ -f "$BUILD_DIR/libtcc.a" ]; then
    cp "$BUILD_DIR/libtcc.a" "$BUNDLE_DIR/lib/libtcc.a"
fi
cp -R "$NEWLIB_SYSROOT/include"/. "$BUNDLE_DIR/include"/
# ArmOS owns a few public ABI headers that complement or wrap newlib's generic
# ones.  Keep them in a separate include directory placed before newlib in the
# search path; wrappers such as stdlib.h rely on #include_next and break if they
# overwrite the real newlib header in /opt/tcc/include.
cp -R "$ROOT_DIR/userland/include"/. "$BUNDLE_DIR/include/armos"/
cp -R "$PATCHED_SRC/include"/. "$BUNDLE_DIR/lib/tcc/include"/

echo
echo "Native ArmOS TinyCC bundle built:"
echo "  $BUNDLE_DIR"
echo
file "$BUNDLE_DIR/bin/tcc"
"${ARCH}objdump" -f "$BUNDLE_DIR/bin/tcc"
echo
echo "To stage it into the generated filesystem for testing:"
echo "  rsync -a $BUNDLE_DIR/ $USERFS_ROOT/opt/tcc/"
echo
echo "Keep /opt/tcc/bin out of PATH: /usr/bin/tcc is the ArmOS wrapper that"
echo "adds the startup objects and libraries needed for normal native builds."
