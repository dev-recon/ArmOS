#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_harfbuzz.sh
# Layer: Host tooling / userland bundles
#
# Responsibilities:
# - Fetch and verify the selected upstream HarfBuzz release.
# - Cross-compile the upstream harfbuzz.cc amalgamation as one C++ object.
# - Install only the static library and public C API for target consumers.
#
# Notes:
# - C++ is a host-side implementation detail of this bundle.
# - No C++ runtime, exception support, RTTI or C++ headers enter userfs.
# - GLib, ICU, Graphite, platform backends, tools and subsetting are disabled.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HARFBUZZ_VERSION="${HARFBUZZ_VERSION:-14.2.1}"
HARFBUZZ_ARCHIVE="harfbuzz-${HARFBUZZ_VERSION}.tar.xz"
HARFBUZZ_URL="${HARFBUZZ_URL:-https://github.com/harfbuzz/harfbuzz/releases/download/${HARFBUZZ_VERSION}/${HARFBUZZ_ARCHIVE}}"
HARFBUZZ_SHA512="${HARFBUZZ_SHA512:-481dca68900e57895d3671baf0595c2d3a26f4f08d0db5662e83089f0d0ff6dc0c28c64c2811eb0f812b0525ed428e90db44f091a88bef47ace2f97d8285b013}"

ARCH="${ARCH:-arm-none-eabi-}"
# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

WORK_DIR="${WORK_DIR:-$BUNDLE_BUILD_ROOT/harfbuzz}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
SOURCE_ROOT="$WORK_DIR/source"
BUILD_DIR="$WORK_DIR/build"
BUNDLE_ROOT="$WORK_DIR/bundle"
BUNDLE_PREFIX="$BUNDLE_ROOT/opt/harfbuzz"
BUNDLE_USR_BIN="$BUNDLE_ROOT/usr/bin"
BUNDLE_TCC_INCLUDE="$BUNDLE_ROOT/opt/tcc/include"
ARCHIVE_PATH="${HARFBUZZ_ARCHIVE_PATH:-$DOWNLOAD_DIR/$HARFBUZZ_ARCHIVE}"
SRC_DIR="${SRC_DIR:-$SOURCE_ROOT/harfbuzz}"

FREETYPE_PREFIX="${FREETYPE_PREFIX:-$BUNDLE_BUILD_ROOT/freetype/bundle/opt/freetype}"
THREADS_LIBRARY="${THREADS_LIBRARY:-$TARGET_BUILD_ROOT/userland/out/usr/lib/libthreads.a}"

CXX="${ARCH}g++"
CC="${ARCH}gcc"
AR="${ARCH}ar"
RANLIB="${ARCH}ranlib"
STRIP="${ARCH}strip"
NM="${ARCH}nm"
LIBGCC="${LIBGCC:-$("$CC" $ARM_FLAGS -print-libgcc-file-name)}"
FORMAT_FLAGS=""
if [ "$TARGET_ARCH" = "arm32" ]; then
    # This GCC target spells its 32-bit integer typedefs as long. They are ABI
    # compatible with int, but trigger noisy diagnostics in upstream format
    # strings that use the standard PRI-equivalent 32-bit specifiers.
    FORMAT_FLAGS="-Wno-format"
fi

find_libcxx_include()
{
    local candidate
    local brew_llvm

    if [ -n "${LIBCXX_INCLUDE:-}" ] && [ -f "$LIBCXX_INCLUDE/__config" ]; then
        printf '%s\n' "$LIBCXX_INCLUDE"
        return
    fi

    for candidate in \
        /opt/homebrew/opt/llvm/include/c++/v1 \
        /usr/local/opt/llvm/include/c++/v1 \
        /usr/include/c++/v1 \
        /usr/lib/llvm-*/include/c++/v1; do
        if [ -f "$candidate/__config" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    if command -v brew >/dev/null 2>&1; then
        brew_llvm="$(brew --prefix llvm 2>/dev/null || true)"
        if [ -f "$brew_llvm/include/c++/v1/__config" ]; then
            printf '%s\n' "$brew_llvm/include/c++/v1"
            return
        fi
    fi

    echo "error: LLVM libc++ headers not found; set LIBCXX_INCLUDE" >&2
    echo "Linux:   sudo apt install libc++-dev" >&2
    echo "Homebrew: brew install llvm" >&2
    exit 1
}

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "error: required cross C++ compiler '$CXX' not found in PATH" >&2
    exit 1
fi
LIBCXX_INCLUDE="$(find_libcxx_include)"

verify_archive()
{
    local actual

    actual="$(shasum -a 512 "$ARCHIVE_PATH" | awk '{print $1}')"
    if [ "$actual" != "$HARFBUZZ_SHA512" ]; then
        echo "error: checksum mismatch for $ARCHIVE_PATH" >&2
        echo "expected: $HARFBUZZ_SHA512" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

if [ ! -f "$SRC_DIR/src/harfbuzz.cc" ]; then
    mkdir -p "$DOWNLOAD_DIR"
    if [ ! -f "$ARCHIVE_PATH" ]; then
        curl -L --fail --output "$ARCHIVE_PATH" "$HARFBUZZ_URL"
    fi
    verify_archive
    rm -rf "$SOURCE_ROOT"
    mkdir -p "$SRC_DIR"
    tar -xJf "$ARCHIVE_PATH" -C "$SRC_DIR" --strip-components=1
fi

if [ ! -f "$SRC_DIR/src/harfbuzz.cc" ] ||
   [ ! -f "$SRC_DIR/COPYING" ]; then
    echo "error: HarfBuzz source tree is incomplete: $SRC_DIR" >&2
    exit 1
fi

for dependency in \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$THREADS_LIBRARY" \
    "$NEWLIB_LIBC" \
    "$NEWLIB_LIBM"; do
    if [ ! -f "$dependency" ]; then
        echo "error: HarfBuzz dependency is missing: $dependency" >&2
        exit 1
    fi
done

rm -rf "$BUILD_DIR" "$BUNDLE_ROOT"
mkdir -p "$BUILD_DIR" "$BUNDLE_PREFIX/include/harfbuzz" \
    "$BUNDLE_PREFIX/lib/pkgconfig" "$BUNDLE_USR_BIN" \
    "$BUNDLE_TCC_INCLUDE/harfbuzz"

CXXFLAGS="$ARM_FLAGS $FORMAT_FLAGS -std=c++17 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics \
-fno-use-cxa-atexit -fvisibility=hidden -nostdinc++ \
-DLLONG_MIN=(-9223372036854775807LL-1) \
-DLLONG_MAX=9223372036854775807LL \
-DULLONG_MAX=18446744073709551615ULL \
-DHAVE_FREETYPE -DHAVE_PTHREAD -DHB_NO_SUBSET \
-DHB_NO_GLIB -DHB_NO_ICU -DHB_NO_GRAPHITE2 -DHB_NO_CAIRO \
-DHB_NO_UNISCRIBE -DHB_NO_DIRECTWRITE -DHB_NO_CORETEXT \
-DHB_NO_GDI -DHB_NO_WASM \
-include $ROOT_DIR/tools/harfbuzz-cxx/armos-cxx-compat.h \
-I$ROOT_DIR/tools/harfbuzz-cxx -I$ROOT_DIR/userland/include \
-isystem $LIBCXX_INCLUDE -I$SRC_DIR/src \
-I$FREETYPE_PREFIX/include/freetype2 \
-idirafter $NEWLIB_SYSROOT/include"
CFLAGS="$ARM_FLAGS -std=gnu18 -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -DARM_OS_NEWLIB -D_GNU_SOURCE=200809L \
-I$ROOT_DIR/userland/include -I$BUNDLE_PREFIX/include/harfbuzz \
-I$FREETYPE_PREFIX/include/freetype2 -I$NEWLIB_SYSROOT/include"
LDFLAGS="$ARM_FLAGS -nostdlib -nostartfiles -static \
-Wl,-Ttext=$TARGET_TEXT_ADDRESS -Wl,-e,_start -Wl,--gc-sections \
-Wl,--allow-multiple-definition"

"$CXX" $CXXFLAGS -c "$SRC_DIR/src/harfbuzz.cc" \
    -o "$BUILD_DIR/harfbuzz.o"
"$AR" rcs "$BUNDLE_PREFIX/lib/libharfbuzz.a" "$BUILD_DIR/harfbuzz.o"
"$RANLIB" "$BUNDLE_PREFIX/lib/libharfbuzz.a"

find "$SRC_DIR/src" -maxdepth 1 -type f -name 'hb*.h' \
    ! -name 'hb-cairo.h' \
    ! -name 'hb-coretext.h' \
    ! -name 'hb-directwrite.h' \
    ! -name 'hb-fontations.h' \
    ! -name 'hb-gdi.h' \
    ! -name 'hb-glib.h' \
    ! -name 'hb-gobject.h' \
    ! -name 'hb-gobject-structs.h' \
    ! -name 'hb-gpu.h' \
    ! -name 'hb-graphite2.h' \
    ! -name 'hb-icu.h' \
    ! -name 'hb-subset.h' \
    ! -name 'hb-subset-serialize.h' \
    ! -name 'hb-uniscribe.h' \
    ! -name 'hb-wasm-api.h' \
    -exec cp {} "$BUNDLE_PREFIX/include/harfbuzz/" \;
cp "$SRC_DIR/COPYING" "$BUNDLE_PREFIX/"
cp -R "$BUNDLE_PREFIX/include/harfbuzz/." "$BUNDLE_TCC_INCLUDE/harfbuzz/"
cp -R "$BUNDLE_PREFIX/include/harfbuzz/." "$BUNDLE_TCC_INCLUDE/"

cat > "$BUNDLE_PREFIX/lib/pkgconfig/harfbuzz.pc" <<EOF
prefix=/opt/harfbuzz
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: harfbuzz
Description: OpenType text shaping library
Version: $HARFBUZZ_VERSION
Requires.private: freetype2
Libs: -L\${libdir} -lharfbuzz
Libs.private: -lfreetype -lthreads -lm
Cflags: -I\${includedir}/harfbuzz
EOF

"$CC" $CFLAGS -c "$ROOT_DIR/userland/opt/harfbuzz/test/harfbuzz-test.c" \
    -o "$BUILD_DIR/harfbuzz-test.o"
"$CC" $LDFLAGS -o "$BUNDLE_USR_BIN/harfbuzz-test" \
    $RUNTIME_OBJECTS \
    "$BUILD_DIR/harfbuzz-test.o" \
    "$BUNDLE_PREFIX/lib/libharfbuzz.a" \
    "$FREETYPE_PREFIX/lib/libfreetype.a" \
    "$THREADS_LIBRARY" \
    "$NEWLIB_LIBM" \
    "$NEWLIB_LIBC" \
    "$LIBGCC"
"$STRIP" --strip-all "$BUNDLE_USR_BIN/harfbuzz-test" || true

if "$NM" -u "$BUNDLE_PREFIX/lib/libharfbuzz.a" |
   grep -Eq '(__cxa|_ZSt|_Zn[wa]|_Zdl|__gxx_personality|_Unwind_)'; then
    echo "error: libharfbuzz.a unexpectedly depends on a C++ runtime" >&2
    exit 1
fi

echo
echo "ArmOS HarfBuzz bundle built:"
echo "  $BUNDLE_ROOT"
