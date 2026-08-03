#!/usr/bin/env bash
# build_raspberry_sd.sh - build an architecture/platform pair for Raspberry Pi.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# shellcheck source=tools/armos_config.sh
source "$ROOT_DIR/tools/armos_config.sh"

ARCH="${ARCH:-}"
TARGET_ARCH="${TARGET_ARCH:-arm64}"
TARGET_PLATFORM="${TARGET_PLATFORM:-raspi3}"
if [ "${ARMOS_BUILD_LOCK_HELD:-0}" != "1" ]; then
    export ARMOS_BUILD_LOCK_DIR="$ROOT_DIR/build/$TARGET_ARCH/$TARGET_PLATFORM/.build.lock"
    exec "$ROOT_DIR/tools/with_build_lock.sh" "$0" "$@"
fi

# Fast iteration defaults: reuse the existing newlib/TCC bundles unless the
# caller explicitly asks for them.
BUILD_NEWLIB="${BUILD_NEWLIB:-0}"
BUILD_TCC="${BUILD_TCC:-0}"

PUSH_MODE="${PUSH_MODE:-boot}"
SD_VOLUME="${SD_VOLUME:-}"
KERNEL_NAME="${KERNEL_NAME:-}"
KERNEL_ADDRESS="${KERNEL_ADDRESS:-0x02010000}"
DEVICE_TREE="${DEVICE_TREE:-}"
DTOVERLAY="${DTOVERLAY:-}"
INIT_UART_BAUD="${INIT_UART_BAUD:-115200}"
INIT_UART_CLOCK="${INIT_UART_CLOCK:-}"
RASPI_FIRMWARE_DIR="${RASPI_FIRMWARE_DIR:-${PI2_FIRMWARE_DIR:-$ROOT_DIR/../PI2/firmware/boot}}"
INSTALL_FIRMWARE="${INSTALL_FIRMWARE:-1}"
WRITE_CONFIG="${WRITE_CONFIG:-1}"
RAW_DEVICE="${RAW_DEVICE:-}"
YES="${YES:-0}"
TARGET_MARKER_NAME="armos-target.txt"

usage() {
    cat <<EOF
usage: tools/build_raspberry_sd.sh [--arch arm32|arm64]
                                   [--platform raspi2|raspi3]
                                   [--boot-volume PATH] [--firmware-dir PATH]
                             [--kernel-name NAME] [--device-tree NAME]
                             [--dtoverlay NAME] [--kernel-address ADDR]
                             [--no-firmware]
                             [--raw-device /dev/rdiskN] [--mode boot|image|raw|none]
                             [--yes] [--skip-build]

Defaults:
  mode:        boot
  target:      $TARGET_ARCH/$TARGET_PLATFORM
  boot volume: ${SD_VOLUME:-selected from the target}
  firmware:    $RASPI_FIRMWARE_DIR
  kernel name: kernel8.img for arm64, kernel7.img for arm32

Environment overrides:
  ARCH, TARGET_ARCH, TARGET_PLATFORM, BUILD_NEWLIB, BUILD_ALL_USERLAND,
  BUILD_TCC, BUILD_BSD,
  BUILD_NCURSES, BUILD_NANO, BUILD_XV_DEPS, BUILD_FBVIEW, SD_VOLUME,
  RASPI_FIRMWARE_DIR, KERNEL_NAME,
  KERNEL_ADDRESS, DEVICE_TREE, DTOVERLAY, INIT_UART_BAUD, INIT_UART_CLOCK,
  PUSH_MODE, RAW_DEVICE, INSTALL_FIRMWARE, WRITE_CONFIG

Examples:
  tools/build_raspberry_sd.sh --arch arm64 --platform raspi3
  tools/build_raspberry_sd.sh --arch arm32 --platform raspi3 --mode image
  tools/build_raspberry_sd.sh --arch arm32 --platform raspi2 --skip-build
  tools/build_raspberry_sd.sh --mode raw --raw-device /dev/rdisk4 --yes
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

confirm() {
    local prompt="$1"

    if [ "$YES" = "1" ]; then
        return 0
    fi

    printf "%s [y/N] " "$prompt"
    read -r answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) die "aborted" ;;
    esac
}

ensure_sudo_ready() {
    if sudo -n true 2>/dev/null; then
        return 0
    fi

    if [ ! -t 0 ]; then
        die "raw mode requires sudo; run this command from an interactive terminal"
    fi

    sudo -v
}

write_config_file() {
    local path="$1"

    cat > "$path" <<EOF
kernel=$KERNEL_NAME
kernel_address=$KERNEL_ADDRESS
arm_64bit=$ARM_64BIT
enable_uart=1
uart_2ndstage=1
device_tree=$DEVICE_TREE
init_uart_baud=$INIT_UART_BAUD
EOF
    if [ -n "$DTOVERLAY" ]; then
        printf 'dtoverlay=%s\n' "$DTOVERLAY" >> "$path"
    fi
    if [ -n "$INIT_UART_CLOCK" ]; then
        printf 'init_uart_clock=%s\n' "$INIT_UART_CLOCK" >> "$path"
    fi
}

write_target_marker() {
    local path="$1"

    printf 'target_arch=%s\ntarget_platform=%s\n' \
        "$TARGET_ARCH" "$TARGET_PLATFORM" > "$path"
}

validate_boot_target() {
    local marker="$SD_VOLUME/$TARGET_MARKER_NAME"
    local expected

    [ -f "$marker" ] || die \
        "boot volume has no $TARGET_MARKER_NAME; initialize the complete SD card with --mode raw"
    expected="$(mktemp -t armos-target.XXXXXX)"
    write_target_marker "$expected"
    if ! cmp -s "$expected" "$marker"; then
        echo "error: boot volume target does not match $TARGET_ARCH/$TARGET_PLATFORM" >&2
        echo "card marker:" >&2
        sed 's/^/  /' "$marker" >&2
        rm -f "$expected"
        die "use --mode raw to replace both boot and root filesystems"
    fi
    rm -f "$expected"
}

fat32_partition_offset() {
    local image="$1"
    "${PYTHON:-python3}" -c 'import struct, sys
data = open(sys.argv[1], "rb").read(512)
if len(data) != 512 or data[510:512] != b"\x55\xaa":
    raise SystemExit("image has no MBR signature")
for i in range(4):
    off = 446 + i * 16
    part_type = data[off + 4]
    start = struct.unpack_from("<I", data, off + 8)[0]
    sectors = struct.unpack_from("<I", data, off + 12)[0]
    # Raspberry Pi firmware and host tooling require a standard FAT32 type.
    if part_type in (0x0B, 0x0C) and start and sectors:
        print(start * 512)
        raise SystemExit(0)
raise SystemExit("image has no standard FAT32 boot partition")' "$image"
}

image_mbr_used_sectors() {
    local image="$1"
    "${PYTHON:-python3}" -c 'import struct, sys
data = open(sys.argv[1], "rb").read(512)
if len(data) != 512 or data[510:512] != b"\x55\xaa":
    raise SystemExit("image has no MBR signature")
end = 0
for i in range(4):
    off = 446 + i * 16
    part_type = data[off + 4]
    start = struct.unpack_from("<I", data, off + 8)[0]
    sectors = struct.unpack_from("<I", data, off + 12)[0]
    if part_type and start and sectors:
        end = max(end, start + sectors)
if end == 0:
    raise SystemExit("image has no usable MBR partitions")
print(end)' "$image"
}

raw_write_image() {
    local used_sectors
    local used_mib

    used_sectors="$(image_mbr_used_sectors "$DISK_IMAGE")"
    if [ $((used_sectors % 2048)) -eq 0 ]; then
        used_mib=$((used_sectors / 2048))
        echo "=== Writing raw SD image through last MBR partition ==="
        echo "size: ${used_mib} MiB (${used_sectors} sectors); QEMU-only padding skipped"
        sudo dd if="$DISK_IMAGE" of="$RAW_DEVICE" bs=1m count="$used_mib" conv=sync
    else
        echo "=== Writing raw SD image through last MBR partition ==="
        echo "size: ${used_sectors} sectors; QEMU-only padding skipped"
        sudo dd if="$DISK_IMAGE" of="$RAW_DEVICE" bs=512 count="$used_sectors" conv=sync
    fi
}

copy_to_image_fat() {
    local image_spec="$1"
    local source="$2"
    local target="$3"

    echo "stage: $target"
    mcopy -o -i "$image_spec" "$source" "::$target"
}

copy_boot_firmware_to_dir() {
    local target_dir="$1"
    local file

    for file in bootcode.bin start.elf fixup.dat "$DEVICE_TREE"; do
        cp "$RASPI_FIRMWARE_DIR/$file" "$target_dir/$file"
    done

    if [ -n "$DTOVERLAY" ]; then
        mkdir -p "$target_dir/overlays"
        if [ -f "$RASPI_FIRMWARE_DIR/overlays/overlay_map.dtb" ]; then
            cp "$RASPI_FIRMWARE_DIR/overlays/overlay_map.dtb" \
                "$target_dir/overlays/overlay_map.dtb"
        fi
        cp "$RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo" \
            "$target_dir/overlays/$DTOVERLAY.dtbo"
    fi
}

ensure_image_dir() {
    local image_spec="$1"
    local target="$2"

    if mdir -i "$image_spec" "::$target" >/dev/null 2>&1; then
        return 0
    fi

    echo "stage: mkdir $target"
    mmd -i "$image_spec" "::$target"
}

stage_image_boot_files() {
    local fat_offset
    local image_spec
    local config_tmp
    local marker_tmp

    command -v mcopy >/dev/null 2>&1 || die "mcopy not found; install mtools"
    command -v mmd >/dev/null 2>&1 || die "mmd not found; install mtools"
    command -v mdir >/dev/null 2>&1 || die "mdir not found; install mtools"

    fat_offset="$(fat32_partition_offset "$DISK_IMAGE")"
    image_spec="$DISK_IMAGE@@$fat_offset"

    echo "=== Staging Raspberry Pi boot files into raw image FAT32 partition ==="
    echo "image:  $DISK_IMAGE"
    echo "offset: $fat_offset"

    if [ "$INSTALL_FIRMWARE" = "1" ]; then
        [ -d "$RASPI_FIRMWARE_DIR" ] || die "firmware directory not found: $RASPI_FIRMWARE_DIR"
        for file in bootcode.bin start.elf fixup.dat "$DEVICE_TREE"; do
            [ -f "$RASPI_FIRMWARE_DIR/$file" ] || die "firmware file not found: $RASPI_FIRMWARE_DIR/$file"
            copy_to_image_fat "$image_spec" "$RASPI_FIRMWARE_DIR/$file" "$file"
        done

        if [ -n "$DTOVERLAY" ]; then
            [ -f "$RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo" ] || \
                die "overlay not found: $RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo"
            ensure_image_dir "$image_spec" "overlays"
            if [ -f "$RASPI_FIRMWARE_DIR/overlays/overlay_map.dtb" ]; then
                copy_to_image_fat "$image_spec" \
                    "$RASPI_FIRMWARE_DIR/overlays/overlay_map.dtb" \
                    "overlays/overlay_map.dtb"
            fi
            copy_to_image_fat "$image_spec" \
                "$RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo" \
                "overlays/$DTOVERLAY.dtbo"
        fi
    fi

    copy_to_image_fat "$image_spec" "$KERNEL_IMAGE" "$KERNEL_NAME"

    if [ "$WRITE_CONFIG" = "1" ]; then
        config_tmp="$(mktemp -t armos-raspi-config.XXXXXX)"
        write_config_file "$config_tmp"
        copy_to_image_fat "$image_spec" "$config_tmp" "config.txt"
        rm -f "$config_tmp"
    fi

    marker_tmp="$(mktemp -t armos-target.XXXXXX)"
    write_target_marker "$marker_tmp"
    copy_to_image_fat "$image_spec" "$marker_tmp" "$TARGET_MARKER_NAME"
    rm -f "$marker_tmp"
}

SKIP_BUILD=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --arch)
            [ "$#" -ge 2 ] || die "--arch requires arm32 or arm64"
            TARGET_ARCH="$2"
            shift 2
            ;;
        --platform)
            [ "$#" -ge 2 ] || die "--platform requires raspi2 or raspi3"
            TARGET_PLATFORM="$2"
            shift 2
            ;;
        --boot-volume)
            [ "$#" -ge 2 ] || die "--boot-volume requires a path"
            SD_VOLUME="$2"
            shift 2
            ;;
        --kernel-name)
            [ "$#" -ge 2 ] || die "--kernel-name requires a filename"
            KERNEL_NAME="$2"
            shift 2
            ;;
        --kernel-address)
            [ "$#" -ge 2 ] || die "--kernel-address requires an address"
            KERNEL_ADDRESS="$2"
            shift 2
            ;;
        --device-tree)
            [ "$#" -ge 2 ] || die "--device-tree requires a filename"
            DEVICE_TREE="$2"
            shift 2
            ;;
        --dtoverlay)
            [ "$#" -ge 2 ] || die "--dtoverlay requires a name"
            DTOVERLAY="$2"
            shift 2
            ;;
        --firmware-dir)
            [ "$#" -ge 2 ] || die "--firmware-dir requires a path"
            RASPI_FIRMWARE_DIR="$2"
            shift 2
            ;;
        --no-firmware)
            INSTALL_FIRMWARE=0
            shift
            ;;
        --no-config)
            WRITE_CONFIG=0
            shift
            ;;
        --mode)
            [ "$#" -ge 2 ] || die "--mode requires boot, raw, or none"
            PUSH_MODE="$2"
            shift 2
            ;;
        --raw-device)
            [ "$#" -ge 2 ] || die "--raw-device requires /dev/rdiskN"
            RAW_DEVICE="$2"
            PUSH_MODE=raw
            shift 2
            ;;
        --yes|-y)
            YES=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

ENABLE_NET="${ENABLE_NET:-0}"
ENABLE_GPU="${ENABLE_GPU:-0}"
armos_config_validate "$ROOT_DIR"

case "$PUSH_MODE" in
    boot|image|raw|none) ;;
    *) die "unsupported PUSH_MODE: $PUSH_MODE" ;;
esac

cd "$ROOT_DIR"

case "$TARGET_ARCH/$TARGET_PLATFORM" in
    arm32/raspi2)
        PI_LABEL="Raspberry Pi 2"
        PI_OK_PREFIX="PI2"
        ARM_64BIT=0
        : "${ARCH:=arm-none-eabi-}"
        : "${KERNEL_NAME:=kernel7.img}"
        : "${DEVICE_TREE:=bcm2709-rpi-2-b.dtb}"
        : "${SD_VOLUME:=/Volumes/RASPI2}"
        ;;
    arm32/raspi3)
        PI_LABEL="Raspberry Pi 3 (AArch32)"
        PI_OK_PREFIX="PI3"
        ARM_64BIT=0
        : "${ARCH:=arm-none-eabi-}"
        : "${KERNEL_NAME:=kernel7.img}"
        : "${DEVICE_TREE:=bcm2710-rpi-3-b-plus.dtb}"
        : "${SD_VOLUME:=/Volumes/RASPI3}"
        ;;
    arm64/raspi3)
        PI_LABEL="Raspberry Pi 3 (AArch64)"
        PI_OK_PREFIX="PI3"
        ARM_64BIT=1
        : "${ARCH:=aarch64-elf-}"
        : "${KERNEL_NAME:=kernel8.img}"
        : "${DEVICE_TREE:=bcm2710-rpi-3-b-plus.dtb}"
        : "${SD_VOLUME:=/Volumes/RASPI3}"
        ;;
    *)
        die "unsupported Raspberry Pi target: $TARGET_ARCH/$TARGET_PLATFORM"
        ;;
esac

IMAGE_SUFFIX="$TARGET_ARCH-$TARGET_PLATFORM"
KERNEL_IMAGE="$ROOT_DIR/build/images/kernel-$IMAGE_SUFFIX.bin"
DISK_IMAGE="$ROOT_DIR/build/images/disk-$IMAGE_SUFFIX.img"

if [ "$TARGET_PLATFORM" = "raspi3" ] && [ "$DEVICE_TREE" = "bcm2709-rpi-2-b.dtb" ]; then
    die "raspi3 requires a Pi 3 device tree; use bcm2710-rpi-3-b-plus.dtb"
fi

if [ "$SKIP_BUILD" != "1" ]; then
    echo "=== Building ArmOS for $PI_LABEL ==="
    echo "BUILD_NEWLIB=$BUILD_NEWLIB BUILD_TCC=$BUILD_TCC"
    ARCH="$ARCH" \
    TARGET_ARCH="$TARGET_ARCH" \
    TARGET_PLATFORM="$TARGET_PLATFORM" \
    BUILD_NEWLIB="$BUILD_NEWLIB" \
    BUILD_TCC="$BUILD_TCC" \
    ./build.sh
fi

[ -f "$KERNEL_IMAGE" ] || die "kernel image not found: $KERNEL_IMAGE"
case "$PUSH_MODE" in
    image|raw)
        [ -f "$DISK_IMAGE" ] || die "disk image not found: $DISK_IMAGE"
        ;;
esac

case "$PUSH_MODE" in
    none)
        echo "=== Build complete; SD push skipped ==="
        ;;
    image)
        stage_image_boot_files
        sync
        echo "${PI_OK_PREFIX}_SD_IMAGE_OK"
        ;;
    boot)
        [ -d "$SD_VOLUME" ] || die "boot volume not mounted: $SD_VOLUME"
        [ -w "$SD_VOLUME" ] || die "boot volume is not writable: $SD_VOLUME"
        validate_boot_target

        echo "=== Boot partition update only ==="
        echo "note: kernel/firmware will be updated, but the SD root filesystem will not."
        echo "note: userland changes such as a rebuilt mtop require --mode raw."

        if [ "$INSTALL_FIRMWARE" = "1" ]; then
            [ -d "$RASPI_FIRMWARE_DIR" ] || die "firmware directory not found: $RASPI_FIRMWARE_DIR"
            for file in bootcode.bin start.elf fixup.dat "$DEVICE_TREE"; do
                [ -f "$RASPI_FIRMWARE_DIR/$file" ] || die "firmware file not found: $RASPI_FIRMWARE_DIR/$file"
            done
            if [ -n "$DTOVERLAY" ]; then
                [ -f "$RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo" ] || \
                    die "overlay not found: $RASPI_FIRMWARE_DIR/overlays/$DTOVERLAY.dtbo"
            fi

            echo "=== Copying $PI_LABEL firmware to SD boot volume ==="
            echo "source: $RASPI_FIRMWARE_DIR"
            echo "target: $SD_VOLUME"
            echo "files:  bootcode.bin start.elf fixup.dat $DEVICE_TREE"
            if [ -n "$DTOVERLAY" ]; then
                echo "overlay: $DTOVERLAY.dtbo"
            fi
            copy_boot_firmware_to_dir "$SD_VOLUME"
        fi

        echo "=== Copying $PI_LABEL kernel to SD boot volume ==="
        echo "source: $KERNEL_IMAGE"
        echo "target: $SD_VOLUME/$KERNEL_NAME"
        cp "$KERNEL_IMAGE" "$SD_VOLUME/$KERNEL_NAME"

        if [ "$WRITE_CONFIG" = "1" ]; then
            echo "=== Writing ArmOS config.txt ==="
            if [ -f "$SD_VOLUME/config.txt" ] && [ ! -f "$SD_VOLUME/config.txt.before-armos" ]; then
                cp "$SD_VOLUME/config.txt" "$SD_VOLUME/config.txt.before-armos"
            fi
            write_config_file "$SD_VOLUME/config.txt"
        fi

        if command -v dot_clean >/dev/null 2>&1; then
            dot_clean "$SD_VOLUME" >/dev/null 2>&1 || true
        fi
        sync
        echo "${PI_OK_PREFIX}_SD_BOOT_OK"
        ;;
    raw)
        [ -n "$RAW_DEVICE" ] || die "raw mode requires --raw-device /dev/rdiskN"
        [ -e "$RAW_DEVICE" ] || die "raw device not found: $RAW_DEVICE"
        case "$RAW_DEVICE" in
            /dev/rdisk*) ;;
            *) die "refusing non-raw macOS disk path; expected /dev/rdiskN" ;;
        esac

        echo "=== About to overwrite SD card ==="
        echo "image:  $DISK_IMAGE"
        echo "device: $RAW_DEVICE"
        echo "note: raw mode writes the full ArmOS SD image, including boot FAT32 and ext2 root."
        confirm "This will destroy all data on $RAW_DEVICE. Continue?"

        ensure_sudo_ready
        stage_image_boot_files
        diskutil unmountDisk "$RAW_DEVICE" >/dev/null
        raw_write_image
        sync
        diskutil eject "$RAW_DEVICE" >/dev/null || true
        echo "${PI_OK_PREFIX}_SD_RAW_OK"
        ;;
esac

echo "Kernel image: $KERNEL_IMAGE"
if [ -f "$DISK_IMAGE" ]; then
    echo "Disk image:   $DISK_IMAGE"
else
    echo "Disk image:   $DISK_IMAGE (not built)"
fi
