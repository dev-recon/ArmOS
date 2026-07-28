#!/usr/bin/env bash
# Smoke tests for the ArmOS configuration precedence and validation rules.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_CONFIG="$(mktemp -t armos-config-test.XXXXXX)"
trap 'rm -f "$TMP_CONFIG"' EXIT

printf '%s\n' \
    'TARGET_ARCH=arm64' \
    'TARGET_PLATFORM=qemu-virt' \
    'KEYBOARD_LAYOUT=fr-mac' \
    'ENABLE_NET=yes' \
    'ENABLE_HDMI=no' \
    'ENABLE_ILI9341=no' \
    'ENABLE_USB=no' \
    'BUILD_NCURSES=yes' \
    'BUILD_NANO=yes' > "$TMP_CONFIG"

resolved="$(ARMOS_CONFIG="$TMP_CONFIG" bash -c '
    ROOT_DIR="$1"
    source "$1/tools/armos_config.sh"
    armos_config_validate "$1"
    printf "%s/%s keymap=%s net=%s hdmi=%s ili9341=%s usb=%s nano=%s\n" \
        "$TARGET_ARCH" "$TARGET_PLATFORM" "$KEYBOARD_LAYOUT" "$ENABLE_NET" \
        "$ENABLE_HDMI" "$ENABLE_ILI9341" "$ENABLE_USB" "$BUILD_NANO"
' _ "$ROOT_DIR")"
[ "$resolved" = 'arm64/qemu-virt keymap=fr-mac net=1 hdmi=0 ili9341=0 usb=0 nano=1' ]

resolved="$(TARGET_ARCH=arm32 ARMOS_CONFIG="$TMP_CONFIG" bash -c '
    ROOT_DIR="$1"
    source "$1/tools/armos_config.sh"
    printf "%s/%s\n" "$TARGET_ARCH" "$TARGET_PLATFORM"
' _ "$ROOT_DIR")"
[ "$resolved" = 'arm32/qemu-virt' ]

printf '%s\n' \
    'TARGET_ARCH=arm64' \
    'TARGET_PLATFORM=raspi3' \
    'ENABLE_HDMI=yes' \
    'ENABLE_ILI9341=no' \
    'ENABLE_USB=yes' \
    'HDMI_WIDTH=1280' \
    'HDMI_HEIGHT=720' > "$TMP_CONFIG"
ARMOS_CONFIG="$TMP_CONFIG" "$ROOT_DIR/tools/armos_config.sh" --show >/dev/null

printf '%s\n' \
    'TARGET_ARCH=arm32' \
    'TARGET_PLATFORM=raspi2' \
    'ENABLE_HDMI=yes' \
    'ENABLE_ILI9341=no' \
    'ENABLE_USB=yes' \
    'HDMI_WIDTH=1280' \
    'HDMI_HEIGHT=720' > "$TMP_CONFIG"
ARMOS_CONFIG="$TMP_CONFIG" "$ROOT_DIR/tools/armos_config.sh" --show >/dev/null

printf '%s\n' \
    'TARGET_ARCH=arm64' \
    'TARGET_PLATFORM=raspi3' \
    'ENABLE_HDMI=yes' \
    'ENABLE_ILI9341=yes' > "$TMP_CONFIG"
ARMOS_CONFIG="$TMP_CONFIG" "$ROOT_DIR/tools/armos_config.sh" --show >/dev/null

printf 'UNKNOWN_OPTION=yes\n' > "$TMP_CONFIG"
if ARMOS_CONFIG="$TMP_CONFIG" "$ROOT_DIR/tools/armos_config.sh" --show >/dev/null 2>&1; then
    echo "configuration accepted an unknown key" >&2
    exit 1
fi
if ARMOS_CONFIG="$TMP_CONFIG" make -s -C "$ROOT_DIR" config >/dev/null 2>&1; then
    echo "make accepted an unknown configuration key" >&2
    exit 1
fi

if make -s -C "$ROOT_DIR" \
    ARMOS_CONFIG="$TMP_CONFIG.missing" config >/dev/null 2>&1; then
    echo "make accepted a missing explicit configuration file" >&2
    exit 1
fi

printf '%s\n' \
    'TARGET_ARCH=arm64' \
    'TARGET_PLATFORM=qemu-virt' \
    'KEYBOARD_LAYOUT=be' > "$TMP_CONFIG"
if ARMOS_CONFIG="$TMP_CONFIG" "$ROOT_DIR/tools/armos_config.sh" --show \
    >/dev/null 2>&1; then
    echo "configuration accepted an unsupported keyboard layout" >&2
    exit 1
fi

echo ARMOS_CONFIG_TEST_OK
