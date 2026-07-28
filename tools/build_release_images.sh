#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/build_release_images.sh
# Layer: Host tooling / release packaging
#
# Responsibilities:
# - Build complete ARM32 and ARM64 QEMU reference images.
# - Validate filesystem consistency and release-image hygiene.
# - Package bootable release assets and generate SHA-256 checksums.
#
# Notes:
# - Archives preserve the build/images layout expected by boot.sh.
# - Target work trees are independent and may be built concurrently.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

OUTPUT_DIR="$ROOT_DIR/images"
SKIP_BUILD=0
REQUESTED_VERSION=""
WORK_DIR=""

usage()
{
    cat <<'EOF'
usage: tools/build_release_images.sh [OPTIONS]

Build and package the ARM32 and ARM64 QEMU reference images.

Options:
  --version VERSION    Require VERSION to match ARMOS_VERSION in Makefile.
  --output-dir DIR     Write archives and SHA256SUMS to DIR (default: images).
  --skip-build         Package existing build/images artifacts.
  -h, --help           Show this help.

Normal release build:
  ./tools/build_release_images.sh --version 0.7.4

Repackage already-built and audited images:
  ./tools/build_release_images.sh --skip-build
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)
            shift
            [ "$#" -gt 0 ] || {
                echo "error: --version requires a value" >&2
                exit 2
            }
            REQUESTED_VERSION="$1"
            ;;
        --output-dir)
            shift
            [ "$#" -gt 0 ] || {
                echo "error: --output-dir requires a directory" >&2
                exit 2
            }
            OUTPUT_DIR="$1"
            ;;
        --skip-build)
            SKIP_BUILD=1
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

case "$OUTPUT_DIR" in
    /*) ;;
    *) OUTPUT_DIR="$ROOT_DIR/$OUTPUT_DIR" ;;
esac

ARMOS_VERSION="$(
    awk '$1 == "ARMOS_VERSION" && $2 == ":=" { print $3; exit }' \
        "$ROOT_DIR/Makefile"
)"
if [ -z "$ARMOS_VERSION" ]; then
    echo "error: cannot read ARMOS_VERSION from Makefile" >&2
    exit 1
fi
case "$ARMOS_VERSION" in
    v*)
        echo "error: ARMOS_VERSION must not use the retired 'v' prefix" >&2
        exit 1
        ;;
    *[!0-9.]*|'')
        echo "error: invalid ARMOS_VERSION: $ARMOS_VERSION" >&2
        exit 1
        ;;
esac
if [ -n "$REQUESTED_VERSION" ] &&
   [ "$REQUESTED_VERSION" != "$ARMOS_VERSION" ]; then
    echo "error: requested version $REQUESTED_VERSION does not match Makefile version $ARMOS_VERSION" >&2
    exit 1
fi

find_e2fs_tool()
{
    local tool="$1"
    local candidate
    local prefix

    if command -v "$tool" >/dev/null 2>&1; then
        command -v "$tool"
        return 0
    fi
    if command -v brew >/dev/null 2>&1; then
        prefix="$(brew --prefix e2fsprogs 2>/dev/null || true)"
        if [ -x "$prefix/sbin/$tool" ]; then
            printf '%s\n' "$prefix/sbin/$tool"
            return 0
        fi
    fi
    for candidate in \
        "/opt/homebrew/opt/e2fsprogs/sbin/$tool" \
        "/usr/local/opt/e2fsprogs/sbin/$tool" \
        "/home/linuxbrew/.linuxbrew/opt/e2fsprogs/sbin/$tool"; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

E2FSCK="$(find_e2fs_tool e2fsck || true)"
DEBUGFS="$(find_e2fs_tool debugfs || true)"
if [ -z "$E2FSCK" ] || [ -z "$DEBUGFS" ]; then
    echo "error: e2fsck and debugfs are required; install e2fsprogs before packaging a release" >&2
    exit 1
fi
for tool in awk cp dd grep gzip mkdir mktemp rm tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required tool '$tool' not found" >&2
        exit 1
    }
done
if command -v sha256sum >/dev/null 2>&1; then
    SHA256_KIND=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    SHA256_KIND=shasum
else
    echo "error: sha256sum or shasum is required" >&2
    exit 1
fi
if tar --version 2>/dev/null | grep -q '^bsdtar '; then
    TAR_OWNER_ARGS=(--uid 0 --gid 0 --uname root --gname root)
else
    TAR_OWNER_ARGS=(--owner=0 --group=0 --numeric-owner)
fi

mkdir -p "$OUTPUT_DIR" "$ROOT_DIR/build"
WORK_DIR="$(mktemp -d "$ROOT_DIR/build/release-images.XXXXXX")"
cleanup()
{
    if [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT INT TERM

ARCHIVE_ARM32="$OUTPUT_DIR/ArmOS-$ARMOS_VERSION-qemu-virt-arm32.tar.gz"
ARCHIVE_ARM64="$OUTPUT_DIR/ArmOS-$ARMOS_VERSION-qemu-virt-arm64.tar.gz"
KERNEL_ARM32="$OUTPUT_DIR/kernel-arm32-qemu-virt.bin"
KERNEL_ARM64="$OUTPUT_DIR/kernel-arm64-qemu-virt.bin"
CHECKSUMS="$OUTPUT_DIR/SHA256SUMS"
rm -f \
    "$ARCHIVE_ARM32" "$ARCHIVE_ARM64" \
    "$KERNEL_ARM32" "$KERNEL_ARM64" \
    "$CHECKSUMS"

validate_ext2_partition()
{
    local disk="$1"
    local architecture="$2"
    local rootfs="$WORK_DIR/rootfs-$architecture.ext2"
    local status

    # QEMU reference disks use an ext2-first layout: LBA 2048, 512 MiB.
    dd if="$disk" of="$rootfs" bs=1048576 skip=1 count=512 2>/dev/null
    set +e
    "$E2FSCK" -f -n "$rootfs"
    status=$?
    set -e
    if [ "$status" -gt 1 ]; then
        echo "error: ext2 validation failed for $disk (e2fsck status $status)" >&2
        exit 1
    fi
    validate_release_contents "$rootfs" "$architecture"
    rm -f "$rootfs"
}

validate_release_contents()
{
    local rootfs="$1"
    local architecture="$2"
    local path
    local missing=0

    echo "=== Verifying complete $architecture release userland ==="
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        if ! "$DEBUGFS" -R "stat $path" "$rootfs" 2>/dev/null |
            grep -q '^Inode:'; then
            echo "error: $architecture release image is missing $path" >&2
            missing=$((missing + 1))
        fi
    done <<'EOF'
/sbin/init
/sbin/mash
/usr/bin/httpget
/usr/bin/fbview
/usr/bin/tcc
/opt/tcc/bin/tcc
/opt/tcc/include/newlib.h
/opt/tcc/lib/libc.a
/opt/tcc/lib/libm.a
/opt/tcc/lib/libgcc.a
/usr/bin/bmake
/opt/bmake/share/mk/sys.mk
/opt/bsdawk/bin/awk
/opt/bsdsed/bin/sed
/opt/bsddiff/bin/diff
/opt/bsdpatch/bin/patch
/opt/bsdpax/bin/pax
/opt/bsdm4/bin/m4
/opt/bsdelftools/bin/elftools
/opt/nano/bin/nano
/opt/ncurses/lib/libncurses.a
/opt/zlib/lib/libz.a
/opt/libjpeg/lib/libjpeg.a
/opt/libpng/lib/libpng.a
/opt/libtiff/lib/libtiff.a
EOF
    if [ "$missing" -ne 0 ]; then
        echo "error: incomplete $architecture release userland ($missing missing path(s))" >&2
        exit 1
    fi
}

package_target()
{
    local architecture="$1"
    local config="$ROOT_DIR/configs/qemu-virt-$architecture.conf"
    local suffix="$architecture-qemu-virt"
    local kernel="$ROOT_DIR/build/images/kernel-$suffix.bin"
    local disk="$ROOT_DIR/build/images/disk-$suffix.img"
    local archive="$OUTPUT_DIR/ArmOS-$ARMOS_VERSION-qemu-virt-$architecture.tar.gz"
    local published_kernel="$OUTPUT_DIR/kernel-$architecture-qemu-virt.bin"
    local stage="$WORK_DIR/stage-$architecture"

    if [ "$SKIP_BUILD" -eq 0 ]; then
        echo "=== Building ArmOS $ARMOS_VERSION QEMU $architecture ==="
        ARMOS_CONFIG="$config" BUILD_ALL_USERLAND=1 "$ROOT_DIR/build.sh"
    fi

    for artifact in "$kernel" "$disk"; do
        if [ ! -f "$artifact" ]; then
            echo "error: missing release artifact: $artifact" >&2
            exit 1
        fi
    done

    echo "=== Validating QEMU $architecture release artifacts ==="
    "$ROOT_DIR/tools/check_release_image_hygiene.sh" "$kernel" "$disk"
    validate_ext2_partition "$disk" "$architecture"
    cp "$kernel" "$published_kernel"

    mkdir -p "$stage/build/images"
    cp "$kernel" "$stage/build/images/"
    cp "$disk" "$stage/build/images/"
    "$ROOT_DIR/tools/check_release_image_hygiene.sh" "$stage"

    echo "=== Packaging $(basename "$archive") ==="
    COPYFILE_DISABLE=1 tar "${TAR_OWNER_ARGS[@]}" \
        -C "$stage" -cf - build/images |
        gzip -n > "$archive"
    tar -tzf "$archive" >/dev/null
    "$ROOT_DIR/tools/check_release_image_hygiene.sh" "$archive"
}

package_target arm32
package_target arm64

echo "=== Generating SHA256SUMS ==="
(
    cd "$OUTPUT_DIR"
    if [ "$SHA256_KIND" = sha256sum ]; then
        sha256sum \
            "$(basename "$ARCHIVE_ARM32")" \
            "$(basename "$ARCHIVE_ARM64")" \
            "$(basename "$KERNEL_ARM32")" \
            "$(basename "$KERNEL_ARM64")"
    else
        shasum -a 256 \
            "$(basename "$ARCHIVE_ARM32")" \
            "$(basename "$ARCHIVE_ARM64")" \
            "$(basename "$KERNEL_ARM32")" \
            "$(basename "$KERNEL_ARM64")"
    fi
) > "$CHECKSUMS"

echo
echo "ArmOS $ARMOS_VERSION release images ready:"
echo "  $ARCHIVE_ARM32"
echo "  $ARCHIVE_ARM64"
echo "  $KERNEL_ARM32"
echo "  $KERNEL_ARM64"
echo "  $CHECKSUMS"
