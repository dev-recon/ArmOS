#!/bin/bash
# boot.sh - boot an existing platform kernel + disk image without rebuilding.

set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

. "$ROOT_DIR/tools/qemu_helpers.sh"
. "$ROOT_DIR/tools/qemu_platform_env.sh"

if [ "$ENABLE_GPU" = 1 ]; then
    exec "$ROOT_DIR/boot-graphics.sh" "$@"
fi
if [ "$ENABLE_NET" = 1 ]; then
    exec "$ROOT_DIR/boot-net.sh" "$@"
fi

QEMU="$(select_arm_qemu "${1:-}" "$ROOT_DIR" "$TARGET_ARCH")"
SMP_CPUS="${SMP_CPUS:-${QEMU_SMP}}"
QEMU_DATA_DIR="$(find_qemu_data_dir "$QEMU")"

if [ ! -f "${QEMU_KERNEL_IMAGE}" ]; then
    echo "Error: kernel image not found: ${QEMU_KERNEL_IMAGE}"
    echo "Run ./run.sh or ./build.sh for TARGET_PLATFORM=${TARGET_PLATFORM} first."
    exit 1
fi

if [ "${QEMU_BLOCK_ENABLED}" != "0" ] && [ ! -f "${QEMU_DISK_IMAGE}" ]; then
    echo "Error: disk image not found: ${QEMU_DISK_IMAGE}"
    echo "Run ./run.sh or ./build.sh for TARGET_PLATFORM=${TARGET_PLATFORM} first."
    exit 1
fi

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "Error: QEMU binary '$QEMU' not found"
    exit 1
fi
require_qemu_version "$QEMU"

echo "=== Booting existing ${QEMU_KERNEL_IMAGE} ==="
echo "QEMU: $("$QEMU" --version | head -n 1)"
echo "Platform: ${TARGET_ARCH}/${TARGET_PLATFORM}"
echo "Machine: ${QEMU_MACHINE}, CPU: ${QEMU_CPU}"
echo "Memory: ${QEMU_MEMORY}"
echo "Kernel: ${QEMU_KERNEL_IMAGE}"
if [ "${QEMU_BLOCK_ENABLED}" != "0" ]; then
    echo "Disk: ${QEMU_DISK_IMAGE}"
    if [ -n "${QEMU_BLOCK_DEVICE}" ]; then
        echo "Block: ${QEMU_BLOCK_IF} ${QEMU_BLOCK_DEVICE}"
    else
        echo "Block: ${QEMU_BLOCK_IF}"
    fi
else
    echo "Block: disabled for this platform"
fi
if [ -n "${QEMU_KERNEL_LOADER_ADDR}" ]; then
    echo "Kernel loader: ${QEMU_KERNEL_LOADER_ADDR}"
fi
echo "SMP: ${SMP_CPUS} CPU(s)"

QEMU_KERNEL_ARGS=(-kernel "${QEMU_KERNEL_IMAGE}")
if [ -n "${QEMU_KERNEL_LOADER_ADDR}" ]; then
    QEMU_KERNEL_ARGS=(-device "loader,file=${QEMU_KERNEL_IMAGE},addr=${QEMU_KERNEL_LOADER_ADDR},cpu-num=0")
else
    QEMU_BOOTARGS="$(qemu_console_bootargs)"
    if [ -n "$QEMU_BOOTARGS" ]; then
        QEMU_KERNEL_ARGS+=(-append "$QEMU_BOOTARGS")
    fi
fi

QEMU_ARGS=(-M "${QEMU_MACHINE}" -cpu "${QEMU_CPU}" -m "${QEMU_MEMORY}" -smp "${SMP_CPUS}")
if [ -n "${QEMU_DATA_DIR}" ]; then
    QEMU_ARGS+=(-L "${QEMU_DATA_DIR}")
fi
if [ "${QEMU_BLOCK_ENABLED}" != "0" ]; then
    case "${QEMU_BLOCK_IF}" in
        sd)
            QEMU_ARGS+=(-drive "file=${QEMU_DISK_IMAGE},if=sd,format=raw")
            ;;
        virtio-mmio)
            QEMU_ARGS+=(-drive "file=${QEMU_DISK_IMAGE},if=none,format=raw,id=hd0" -device "${QEMU_BLOCK_DEVICE}")
            ;;
        *)
            echo "Error: unsupported QEMU_BLOCK_IF='${QEMU_BLOCK_IF}'" >&2
            exit 1
            ;;
    esac
fi
QEMU_ARGS+=("${QEMU_KERNEL_ARGS[@]}" -nographic)

"$QEMU" "${QEMU_ARGS[@]}"
