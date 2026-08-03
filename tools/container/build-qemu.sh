#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0. See LICENSE for details.
#
# Build the pinned Linux QEMU/VirGL host tools from the ArmOS container without
# colliding with a native macOS, Windows or Linux QEMU built from the checkout.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU_VERSION=10.0.2
container_arch="${ARMOS_CONTAINER_ARCH:-$(uname -m)}"

case "$container_arch" in
    amd64|x86_64) host_id=linux-amd64 ;;
    arm64|aarch64) host_id=linux-arm64 ;;
    *)
        echo "error: unsupported container architecture: $container_arch" >&2
        exit 2
        ;;
esac

artifact_dir="$ROOT_DIR/build/host-tools/qemu/$host_id/qemu-$QEMU_VERSION"
work_dir="${ARMOS_CONTAINER_QEMU_WORK_DIR:-/tmp/armos-qemu/$host_id/qemu-$QEMU_VERSION}"
download_dir="${ARMOS_CONTAINER_QEMU_DOWNLOAD_DIR:-$ROOT_DIR/build/downloads}"
prefix="$artifact_dir/install"
contract_file="$artifact_dir/.armos-container-qemu.contract"
qemu_arm="$prefix/bin/qemu-system-arm"
qemu_arm64="$prefix/bin/qemu-system-aarch64"

contract="$({
    printf 'qemu=%s\n' "$QEMU_VERSION"
    printf 'host=%s\n' "$host_id"
    sha256sum "$ROOT_DIR/tools/build_qemu_10_0_2.sh"
    find "$ROOT_DIR/tools/patches/qemu-$QEMU_VERSION" -type f -print0 |
        sort -z | xargs -0 sha256sum
    pkg-config --modversion sdl2 epoxy virglrenderer slirp
} | sha256sum | awk '{print $1}')"

qemu_is_valid()
{
    local binary="$1"
    local device_help display_help version_output

    [ -x "$binary" ] || return 1
    version_output="$("$binary" --version 2>/dev/null)" || return 1
    grep -q "^QEMU emulator version $QEMU_VERSION" <<<"$version_output" ||
        return 1
    display_help="$("$binary" -display help 2>/dev/null)" || return 1
    grep -qx sdl <<<"$display_help" || return 1
    device_help="$("$binary" -device help 2>/dev/null)" || return 1
    grep -q 'name "virtio-gpu-gl-device"' <<<"$device_help" || return 1
}

if [ -f "$contract_file" ] &&
   [ "$(cat "$contract_file")" = "$contract" ] &&
   qemu_is_valid "$qemu_arm" && qemu_is_valid "$qemu_arm64"; then
    echo "=== Container QEMU cache: reusing $prefix ==="
else
    echo "=== Building container QEMU $QEMU_VERSION for $host_id ==="
    mkdir -p "$artifact_dir" "$download_dir"
    WORK_DIR="$work_dir" DOWNLOAD_DIR="$download_dir" PREFIX="$prefix" \
        "$ROOT_DIR/tools/build_qemu_10_0_2.sh"
    printf '%s\n' "$contract" > "$contract_file"
fi

cat <<EOF

Container QEMU is ready:
  $qemu_arm
  $qemu_arm64

This Linux build is isolated from build/qemu-$QEMU_VERSION/install, which
remains reserved for the native host build.
EOF
