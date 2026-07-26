#!/usr/bin/env bash
# Build a repo-local newlib sysroot for arm-os userland experiments.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${TARGET:-arm-none-eabi}"
ARCH="${ARCH:-${TARGET}-}"
NEWLIB_VERSION="${NEWLIB_VERSION:-4.4.0.20231231}"
ARCHIVE="${NEWLIB_ARCHIVE:-$ROOT_DIR/newlib-$NEWLIB_VERSION.tar.gz}"
NEWLIB_URL="${NEWLIB_URL:-https://sourceware.org/pub/newlib/newlib-$NEWLIB_VERSION.tar.gz}"
PATCH_DIR="${NEWLIB_PATCH_DIR:-$ROOT_DIR/patches/newlib-$NEWLIB_VERSION}"
REPRODUCIBLE_ROOT="${ARMOS_REPRODUCIBLE_ROOT:-/usr/src/armos}"
REPRODUCIBLE_FLAGS="\
-ffile-prefix-map=$ROOT_DIR=$REPRODUCIBLE_ROOT \
-fmacro-prefix-map=$ROOT_DIR=$REPRODUCIBLE_ROOT \
-fdebug-prefix-map=$ROOT_DIR=$REPRODUCIBLE_ROOT"

export PATH="/opt/homebrew/Cellar/arm-none-eabi-gcc/15.1.0/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:$PATH"

case "$TARGET" in
    arm-none-eabi)
        TARGET_ARCH="${TARGET_ARCH:-arm32}"
        TARGET_CFLAGS="-mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft -Os -D__DYNAMIC_REENT__ $REPRODUCIBLE_FLAGS"
        ;;
    aarch64-elf)
        TARGET_ARCH="${TARGET_ARCH:-arm64}"
        TARGET_CFLAGS="-mcpu=cortex-a53 -Os -D__DYNAMIC_REENT__ $REPRODUCIBLE_FLAGS"
        ;;
    *)
        echo "Error: unsupported newlib target: $TARGET" >&2
        exit 1
        ;;
esac

TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"
TARGET_BUILD_ROOT="${TARGET_BUILD_ROOT:-$ROOT_DIR/build/$TARGET_ARCH/$TARGET_PLATFORM}"
BUILD_ROOT="${NEWLIB_BUILD_ROOT:-$TARGET_BUILD_ROOT/newlib-build}"
INSTALL_ROOT="${NEWLIB_INSTALL_ROOT:-$TARGET_BUILD_ROOT/newlib-sysroot}"
SYSROOT="$INSTALL_ROOT/$TARGET"
SRC_DIR="$BUILD_ROOT/src/newlib-$NEWLIB_VERSION"
OBJ_DIR="$BUILD_ROOT/obj-$TARGET"
PATCH_SERIES="$PATCH_DIR/series"
PATCH_STAMP="$SRC_DIR/.arm-os-patches-applied"
REPRODUCIBLE_STAMP="$SYSROOT/.armos-reproducible-paths-v1"
OBJECT_CONTRACT_STAMP="$OBJ_DIR/.armos-build-contract"

if ! command -v "${ARCH}gcc" >/dev/null 2>&1; then
    echo "Error: ${ARCH}gcc not found in PATH" >&2
    exit 1
fi

if [ ! -f "$ARCHIVE" ]; then
    if ! command -v curl >/dev/null 2>&1; then
        echo "Error: newlib archive not found and curl is unavailable: $ARCHIVE" >&2
        echo "Place newlib-$NEWLIB_VERSION.tar.gz at the repository root, set NEWLIB_ARCHIVE, or install curl." >&2
        exit 1
    fi

    echo "=== Downloading newlib $NEWLIB_VERSION ==="
    echo "Source: $NEWLIB_URL"
    curl -L "$NEWLIB_URL" -o "$ARCHIVE"
fi

BUILD_CONTRACT="ArmOS Newlib reproducible paths v1
target=$TARGET
cflags=$TARGET_CFLAGS
retargetable_locking=yes"

mkdir -p "$BUILD_ROOT/src" "$INSTALL_ROOT"
if [ -d "$OBJ_DIR" ] &&
   { [ ! -f "$OBJECT_CONTRACT_STAMP" ] ||
     [ "$(cat "$OBJECT_CONTRACT_STAMP")" != "$BUILD_CONTRACT" ]; }; then
    echo "=== Newlib build contract changed; recreating target objects ==="
    rm -rf "$OBJ_DIR"
fi
mkdir -p "$OBJ_DIR"

patch_state()
{
    if [ ! -f "$PATCH_SERIES" ]; then
        return 0
    fi

    (
        cd "$PATCH_DIR"
        printf '%s\n' "series:"
        cat series
        while IFS= read -r patch_name || [ -n "$patch_name" ]; do
            case "$patch_name" in
                ""|\#*) continue ;;
            esac
            printf '\n%s\n' "$patch_name"
            cat "$patch_name"
        done < series
    ) | shasum -a 256 | awk '{print $1}'
}

EXPECTED_PATCH_STATE="$(patch_state || true)"

if [ -n "$EXPECTED_PATCH_STATE" ] &&
   [ -d "$SRC_DIR" ] &&
   { [ ! -f "$PATCH_STAMP" ] || [ "$(cat "$PATCH_STAMP")" != "$EXPECTED_PATCH_STATE" ]; }; then
    echo "=== Newlib patch series changed; re-extracting clean sources ==="
    rm -rf "$SRC_DIR"
    rm -rf "$BUILD_ROOT"/obj-*
    mkdir -p "$OBJ_DIR"
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "=== Extracting newlib $NEWLIB_VERSION ==="
    tar -xzf "$ARCHIVE" -C "$BUILD_ROOT/src"
fi

if [ -f "$PATCH_SERIES" ]; then
    if [ ! -f "$PATCH_STAMP" ] || [ "$(cat "$PATCH_STAMP")" != "$EXPECTED_PATCH_STATE" ]; then
        echo "=== Applying arm-os newlib patches ==="
        while IFS= read -r patch_name || [ -n "$patch_name" ]; do
            case "$patch_name" in
                ""|\#*) continue ;;
            esac

            patch_file="$PATCH_DIR/$patch_name"
            if [ ! -f "$patch_file" ]; then
                echo "Error: listed newlib patch not found: $patch_file" >&2
                exit 1
            fi

            echo "Applying $patch_name"
            patch -d "$SRC_DIR" -p1 < "$patch_file"
        done < "$PATCH_SERIES"
        printf '%s\n' "$EXPECTED_PATCH_STATE" > "$PATCH_STAMP"
    else
        echo "=== arm-os newlib patches already applied ==="
    fi
fi

echo "=== Configuring newlib for $TARGET ==="
(
    cd "$OBJ_DIR"
    "$SRC_DIR/configure" \
        --target="$TARGET" \
        --prefix="$INSTALL_ROOT" \
        --disable-multilib \
        --disable-newlib-supplied-syscalls \
        --enable-newlib-multithread \
        --enable-newlib-retargetable-locking \
        --enable-newlib-reent-small \
        CFLAGS_FOR_TARGET="$TARGET_CFLAGS"
)

echo "=== Building newlib ==="
make -C "$OBJ_DIR" all-target-newlib

echo "=== Installing newlib into $SYSROOT ==="
make -C "$OBJ_DIR" install-target-newlib
cp "$ROOT_DIR/userland/include/semaphore.h" "$SYSROOT/include/semaphore.h"

if [ ! -f "$SYSROOT/include/stdio.h" ] || [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "Error: expected newlib sysroot files were not produced." >&2
    echo "Missing one of:" >&2
    echo "  $SYSROOT/include/stdio.h" >&2
    echo "  $SYSROOT/lib/libc.a" >&2
    exit 1
fi

printf '%s\n' "$BUILD_CONTRACT" > "$OBJECT_CONTRACT_STAMP"
printf '%s\n' "ArmOS reproducible paths v1: $REPRODUCIBLE_ROOT" \
    > "$REPRODUCIBLE_STAMP"

echo "Newlib sysroot ready:"
echo "  NEWLIB_SYSROOT=$SYSROOT"
