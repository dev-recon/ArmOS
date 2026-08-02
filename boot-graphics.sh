#!/bin/bash
# boot-graphics.sh - boot an existing platform kernel + disk image with a QEMU
# graphics window for framebuffer/GPU experiments.
#
# This script keeps the PL011 UART console on stdio as tty0 and opens a
# virtio-gpu window used by tty1. tty0 is the rescue console and must remain
# usable even if graphical console work regresses.

set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

ENABLE_GPU=1
. "$ROOT_DIR/tools/qemu_helpers.sh"

select_display() {
    local display_help

    if [ -n "${QEMU_DISPLAY:-}" ]; then
        printf '%s\n' "$QEMU_DISPLAY"
        return
    fi

    case "$(uname -s)" in
        Darwin)
            if [ "${QEMU_GPU_ACCEL:-2d}" = "virgl" ]; then
                display_help="$("$QEMU" -display help 2>/dev/null || true)"
                if ! printf '%s\n' "$display_help" | grep -qx sdl; then
                    echo "Error: VirGL on macOS requires QEMU's SDL display backend." >&2
                    echo "Rebuild QEMU with SDL support, or use QEMU_GPU_ACCEL=2d." >&2
                    return 1
                fi
                printf '%s\n' "sdl,show-cursor=off"
            else
                printf '%s\n' "cocoa,show-cursor=off"
            fi
            ;;
        Linux)
            display_help="$("$QEMU" -display help 2>/dev/null || true)"
            if printf '%s\n' "$display_help" | grep -qx gtk; then
                printf '%s\n' "gtk,show-cursor=off"
            elif printf '%s\n' "$display_help" | grep -qx sdl; then
                printf '%s\n' "sdl,show-cursor=off"
            else
                echo "Error: QEMU '$QEMU' has no GTK or SDL window display backend" >&2
                echo "Install libgtk-3-dev and rebuild QEMU with:" >&2
                echo "  ./tools/build_qemu_10_0_2.sh" >&2
                return 1
            fi
            ;;
        *)
            printf '%s\n' "default"
            ;;
    esac
}

. "$ROOT_DIR/tools/qemu_platform_env.sh"
QEMU="$(select_arm_qemu "${1:-}" "$ROOT_DIR" "$TARGET_ARCH")"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "Error: QEMU binary '$QEMU' not found" >&2
    exit 1
fi
if ! QEMU_VERSION_OUTPUT="$("$QEMU" --version 2>&1)"; then
    echo "Error: QEMU '$QEMU' cannot start." >&2
    printf '%s\n' "$QEMU_VERSION_OUTPUT" >&2
    echo "Check its host dynamic-library dependencies." >&2
    exit 1
fi
QEMU_DISPLAY="$(select_display)"
SMP_CPUS="${SMP_CPUS:-${QEMU_SMP}}"
QEMU_DATA_DIR="$(find_qemu_data_dir "$QEMU")"

case "${QEMU_GPU_ACCEL}" in
    2d)
        GPU_DEVICE="${QEMU_GPU_DEVICE}"
        ;;
    virgl)
        QEMU_DEVICE_HELP="$("$QEMU" -device help 2>/dev/null)"
        if ! printf '%s\n' "$QEMU_DEVICE_HELP" |
             grep -q 'name "virtio-gpu-gl-device"'; then
            echo "Error: QEMU '$QEMU' has no virtio-gpu-gl-device." >&2
            echo "Install or build QEMU with virglrenderer support, or use QEMU_GPU_ACCEL=2d." >&2
            exit 1
        fi
        GPU_DEVICE="virtio-gpu-gl-device"
        QEMU_GL_DEVICE_HELP="$("$QEMU" -device virtio-gpu-gl-device,help 2>&1 || true)"
        if printf '%s\n' "$QEMU_GL_DEVICE_HELP" |
             grep -q 'hostmem=<size>'; then
            GPU_DEVICE="${GPU_DEVICE},blob=on,hostmem=${QEMU_GPU_HOSTMEM}"
        fi
        case "${QEMU_DISPLAY}" in
            *,gl=*) ;;
            *)
                if [ "$(uname -s)" = "Darwin" ]; then
                    QEMU_DISPLAY="${QEMU_DISPLAY},gl=core"
                else
                    QEMU_DISPLAY="${QEMU_DISPLAY},gl=on"
                fi
                ;;
        esac
        ;;
    *)
        echo "Error: unsupported QEMU_GPU_ACCEL='${QEMU_GPU_ACCEL}' (expected 2d or virgl)." >&2
        exit 1
        ;;
esac
if [ -n "${GPU_XRES:-}" ] || [ -n "${GPU_YRES:-}" ]; then
    if [ -z "${GPU_XRES:-}" ] || [ -z "${GPU_YRES:-}" ]; then
        echo "Error: GPU_XRES and GPU_YRES must be set together." >&2
        exit 1
    fi
    GPU_DEVICE="${GPU_DEVICE},xres=${GPU_XRES},yres=${GPU_YRES}"
fi

NET_ARGS=()
if [ "$ENABLE_NET" = 1 ]; then
    NET_HOST_ADDR="${NET_HOST_ADDR:-127.0.0.1}"
    NET_HOST_PORT="${NET_HOST_PORT:-2323}"
    NET_GUEST_PORT="${NET_GUEST_PORT:-2323}"
    NET_MAC="${NET_MAC:-52:54:00:12:34:56}"
    NETDEV="user,id=net0,hostfwd=tcp:${NET_HOST_ADDR}:${NET_HOST_PORT}-:${NET_GUEST_PORT}"
    NET_DEVICE="${QEMU_NET_DEVICE_MODEL},netdev=net0,mac=${NET_MAC}"
    NET_ARGS=(-netdev "$NETDEV" -device "$NET_DEVICE")
fi

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

require_qemu_version "$QEMU"

if [ -z "${GPU_DEVICE}" ]; then
    echo "Error: platform '${TARGET_ARCH}/${TARGET_PLATFORM}' does not define a QEMU GPU device"
    exit 1
fi

if [ -z "${QEMU_INPUT_DEVICE}" ]; then
    echo "Error: platform '${TARGET_ARCH}/${TARGET_PLATFORM}' does not define a QEMU input device"
    exit 1
fi

if [ "$ENABLE_NET" = 1 ] && [ -z "${QEMU_NET_DEVICE_MODEL}" ]; then
    echo "Error: platform '${TARGET_ARCH}/${TARGET_PLATFORM}' does not define a QEMU network device"
    exit 1
fi

echo "=== Booting existing ${QEMU_KERNEL_IMAGE} with virtio-gpu ==="
echo "UART console stays on this terminal; graphics output opens in a QEMU window."
echo "QEMU: $(printf '%s\n' "$QEMU_VERSION_OUTPUT" | head -n 1)"
echo "Platform: ${TARGET_ARCH}/${TARGET_PLATFORM}"
echo "Machine: ${QEMU_MACHINE}, CPU: ${QEMU_CPU}"
echo "Memory: ${QEMU_MEMORY}"
echo "Kernel: ${QEMU_KERNEL_IMAGE}"
echo "GPU: ${GPU_DEVICE}, display=${QEMU_DISPLAY}"
echo "GPU acceleration: ${QEMU_GPU_ACCEL}"
if [ "$ENABLE_NET" = 1 ]; then
    echo "NET: ${NET_DEVICE}"
    echo "FWD: ${NET_HOST_ADDR}:${NET_HOST_PORT} -> guest :${NET_GUEST_PORT}"
fi
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
if [ "$ENABLE_NET" = 1 ]; then
    QEMU_ARGS+=("${NET_ARGS[@]}")
fi
QEMU_ARGS+=(
    -device "${GPU_DEVICE}"
    -device "${QEMU_INPUT_DEVICE}"
    -chardev stdio,id=uart0,signal=off
    -serial chardev:uart0
    -monitor none
    "${QEMU_KERNEL_ARGS[@]}"
    -display "${QEMU_DISPLAY}"
)
if [ -n "${QEMU_POINTER_DEVICE}" ]; then
    QEMU_ARGS+=(-device "${QEMU_POINTER_DEVICE}")
fi

"$QEMU" "${QEMU_ARGS[@]}"
