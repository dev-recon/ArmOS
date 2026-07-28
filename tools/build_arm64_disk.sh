#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_arm64_disk.sh
# Layer: Host tooling / ARM64 root filesystem
#
# Responsibilities:
# - Install the AArch64 userland into its target filesystem hierarchy.
# - Build the platform-selected MBR, ext2 root and FAT32 boot layout.
# - Keep ARM32 and ARM64 filesystem paths and non-binary content identical.
#
# Notes:
# - Only generated executable bytes differ between architecture builds.
# - Raspberry Pi firmware staging remains owned by the hardware tools.
# - Platform layout values normally come from the selected platform.mk.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"

if [ "${ARMOS_BUILD_LOCK_HELD:-0}" != "1" ]; then
    export ARMOS_BUILD_LOCK_DIR="$ROOT_DIR/build/arm64/$TARGET_PLATFORM/.build.lock"
    exec "$ROOT_DIR/tools/with_build_lock.sh" "$0" "$@"
fi

OUTPUT="$ROOT_DIR/build/images/disk-arm64-$TARGET_PLATFORM.img"
ROOTFS="$ROOT_DIR/build/images/rootfs-arm64-$TARGET_PLATFORM.ext2"
DISK_WORK_DIR="$ROOT_DIR/build/targets/arm64-$TARGET_PLATFORM/disk"
DISK_BUILD="$DISK_WORK_DIR/standard.img"
EXT2_IMAGE="$DISK_WORK_DIR/ext2.img"
FAT32_IMAGE="$DISK_WORK_DIR/fat32.img"
SKIP_USERLAND=0

FAT32_SIZE_MB=64
EXT2_SIZE_MB=512
DISK_RESERVED_MB=1
MINIMUM_DISK_SIZE_MB=$((DISK_RESERVED_MB + FAT32_SIZE_MB + EXT2_SIZE_MB))
SECTORS_PER_MB=2048

case "$TARGET_PLATFORM" in
    qemu-virt)
        DEFAULT_DISK_LAYOUT=ext2-first
        DEFAULT_HIDDEN_BOOT=0
        DEFAULT_DISK_SIZE_MB=$MINIMUM_DISK_SIZE_MB
        ;;
    raspi3)
        DEFAULT_DISK_LAYOUT=fat32-first
        DEFAULT_HIDDEN_BOOT=0
        DEFAULT_DISK_SIZE_MB=1024
        ;;
    *)
        echo "error: unsupported ARM64 disk platform: $TARGET_PLATFORM" >&2
        exit 2
        ;;
esac

DISK_LAYOUT="${PLATFORM_DISK_LAYOUT:-$DEFAULT_DISK_LAYOUT}"
HIDDEN_BOOT="${PLATFORM_DISK_HIDDEN_BOOT:-$DEFAULT_HIDDEN_BOOT}"
DISK_SIZE_MB="${PLATFORM_DISK_SIZE_MB:-$DEFAULT_DISK_SIZE_MB}"

usage()
{
    cat <<'EOF'
usage: tools/build_arm64_disk.sh [--skip-userland] [--output FILE]

Builds the standard ArmOS disk from target userfs after installing the complete
AArch64 userland. TARGET_PLATFORM selects qemu-virt or raspi3 and its disk
layout; platform.mk values can override the layout, hidden boot flag and size.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-userland)
            SKIP_USERLAND=1
            ;;
        --output)
            shift
            if [ "$#" -eq 0 ]; then
                echo "error: --output requires a file" >&2
                exit 2
            fi
            OUTPUT="$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$SKIP_USERLAND" -eq 0 ]; then
    "$ROOT_DIR/tools/build_arm64_userland.sh" --install
fi

mkdir -p "$(dirname "$OUTPUT")" "$(dirname "$ROOTFS")" "$DISK_WORK_DIR"
rm -f "$EXT2_IMAGE" "$FAT32_IMAGE" "$DISK_BUILD" "$OUTPUT"
make -C "$ROOT_DIR" \
    TARGET_ARCH=arm64 \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    "build/targets/arm64-$TARGET_PLATFORM/disk/ext2.img" \
    "build/targets/arm64-$TARGET_PLATFORM/disk/fat32.img"

if [ "$DISK_SIZE_MB" -lt "$MINIMUM_DISK_SIZE_MB" ]; then
    echo "error: disk size ${DISK_SIZE_MB}MB is smaller than the ${MINIMUM_DISK_SIZE_MB}MB layout" >&2
    exit 1
fi

case "$DISK_LAYOUT" in
    ext2-first)
        EXT2_START_MB=$DISK_RESERVED_MB
        FAT32_START_MB=$((EXT2_START_MB + EXT2_SIZE_MB))
        MBR_LAYOUT_ARGS=()
        ;;
    fat32-first)
        FAT32_START_MB=$DISK_RESERVED_MB
        EXT2_START_MB=$((FAT32_START_MB + FAT32_SIZE_MB))
        MBR_LAYOUT_ARGS=(--fat32-first)
        ;;
    *)
        echo "error: unsupported disk layout: $DISK_LAYOUT" >&2
        exit 2
        ;;
esac

case "$HIDDEN_BOOT" in
    1|yes|true) MBR_LAYOUT_ARGS+=(--hidden-fat32) ;;
    0|no|false) ;;
    *)
        echo "error: invalid hidden boot flag: $HIDDEN_BOOT" >&2
        exit 2
        ;;
esac

EXT2_START_SECTOR=$((EXT2_START_MB * SECTORS_PER_MB))
FAT32_START_SECTOR=$((FAT32_START_MB * SECTORS_PER_MB))
EXT2_SECTORS=$((EXT2_SIZE_MB * SECTORS_PER_MB))
FAT32_SECTORS=$((FAT32_SIZE_MB * SECTORS_PER_MB))

echo "=== Assembling ARM64 $TARGET_PLATFORM disk ==="
echo "layout: $DISK_LAYOUT, hidden boot: $HIDDEN_BOOT, size: ${DISK_SIZE_MB}MB"
dd if=/dev/zero of="$DISK_BUILD" bs=1048576 count="$DISK_SIZE_MB" 2>/dev/null
python3 "$ROOT_DIR/tools/make_mbr.py" "$DISK_BUILD" \
    "$EXT2_START_SECTOR" "$EXT2_SECTORS" \
    "$FAT32_START_SECTOR" "$FAT32_SECTORS" \
    "${MBR_LAYOUT_ARGS[@]}"
dd if="$EXT2_IMAGE" of="$DISK_BUILD" bs=1048576 \
    seek="$EXT2_START_MB" conv=notrunc 2>/dev/null
dd if="$FAT32_IMAGE" of="$DISK_BUILD" bs=1048576 \
    seek="$FAT32_START_MB" conv=notrunc 2>/dev/null

cp "$DISK_BUILD" "$OUTPUT"
cp "$EXT2_IMAGE" "$ROOTFS"

echo "ARM64 $TARGET_PLATFORM disk: $OUTPUT"
echo "ARM64 ext2 root: $ROOTFS"
