#!/usr/bin/env bash
# Shared ArmOS build and launch configuration loader.

# This file is both sourceable by ArmOS scripts and executable for inspecting
# the resolved configuration. Values are parsed as data; the configuration is
# never evaluated as shell code.

ARMOS_CONFIG_KEYS="
TARGET_ARCH TARGET_PLATFORM ARCH CROSS_COMPILE NEWLIB_SYSROOT SMP_CPUS KEYBOARD_LAYOUT
ENABLE_NET ENABLE_WIFI ENABLE_GPU ENABLE_HDMI ENABLE_ILI9341 ENABLE_USB HDMI_WIDTH HDMI_HEIGHT
BUILD_NEWLIB BUILD_ALL_USERLAND BUILD_TCC BUILD_BSD BUILD_NCURSES BUILD_NANO BUILD_EPOLL_SHIM BUILD_PIXMAN BUILD_TLLIST BUILD_UTF8PROC BUILD_FREETYPE BUILD_EXPAT BUILD_FONTCONFIG BUILD_HARFBUZZ BUILD_FCFT BUILD_FOOT BUILD_NUKLEAR
BUILD_XV_DEPS BUILD_FBVIEW BUILD_ZLIB BUILD_LIBJPEG BUILD_LIBPNG BUILD_LIBTIFF
QEMU_MEMORY QEMU_CPU QEMU_MACHINE QEMU_REQUIRED_VERSION QEMU_DISPLAY
NET_HOST_ADDR NET_HOST_PORT NET_GUEST_PORT NET_MAC GPU_XRES GPU_YRES
SD_VOLUME RASPI_FIRMWARE_DIR DEVICE_TREE DTOVERLAY
"

ARMOS_CONFIG_BOOLEAN_KEYS="
ENABLE_NET ENABLE_WIFI ENABLE_GPU ENABLE_HDMI ENABLE_ILI9341 ENABLE_USB
BUILD_NEWLIB BUILD_ALL_USERLAND BUILD_TCC BUILD_BSD BUILD_NCURSES BUILD_NANO BUILD_EPOLL_SHIM BUILD_PIXMAN BUILD_TLLIST BUILD_UTF8PROC BUILD_FREETYPE BUILD_EXPAT BUILD_FONTCONFIG BUILD_HARFBUZZ BUILD_FCFT BUILD_FOOT BUILD_NUKLEAR
BUILD_XV_DEPS BUILD_FBVIEW BUILD_ZLIB BUILD_LIBJPEG BUILD_LIBPNG BUILD_LIBTIFF
"

armos_config_error() {
    printf 'armos.conf: %s\n' "$*" >&2
    return 1
}

armos_config_trim() {
    local value="$1"

    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

armos_config_key_allowed() {
    local allowed

    for allowed in $ARMOS_CONFIG_KEYS; do
        [ "$allowed" = "$1" ] && return 0
    done
    return 1
}

armos_config_is_boolean() {
    local boolean

    for boolean in $ARMOS_CONFIG_BOOLEAN_KEYS; do
        [ "$boolean" = "$1" ] && return 0
    done
    return 1
}

armos_config_normalize_boolean() {
    local key="$1"
    local value="${!key:-}"

    [ -n "$value" ] || return 0
    case "$value" in
        1|yes|true|on) value=1 ;;
        0|no|false|off) value=0 ;;
        *) armos_config_error "$key expects yes/no (or 1/0), got '$value'" ;;
    esac
    printf -v "$key" '%s' "$value"
    export "$key"
}

armos_config_normalize() {
    local key

    for key in $ARMOS_CONFIG_BOOLEAN_KEYS; do
        armos_config_normalize_boolean "$key" || return 1
    done
}

armos_config_load() {
    local root_dir="$1"
    local explicit=0
    local config_file
    local raw line key value quote line_number=0

    if [ "${ARMOS_CONFIG+x}" = x ]; then
        explicit=1
        config_file="$ARMOS_CONFIG"
    else
        config_file="$root_dir/armos.conf"
    fi
    case "$config_file" in
        /*) ;;
        *) config_file="$root_dir/$config_file" ;;
    esac
    ARMOS_CONFIG="$config_file"
    export ARMOS_CONFIG

    if [ ! -f "$config_file" ]; then
        if [ "$explicit" -eq 1 ]; then
            armos_config_error "selected file does not exist: $config_file"
            return 1
        fi
        ARMOS_CONFIG_LOADED=1
        export ARMOS_CONFIG_LOADED
        return 0
    fi

    while IFS= read -r raw || [ -n "$raw" ]; do
        line_number=$((line_number + 1))
        line="$(armos_config_trim "$raw")"
        case "$line" in
            ''|'#'*) continue ;;
        esac
        case "$line" in
            *=*) ;;
            *) armos_config_error "$config_file:$line_number: expected KEY=VALUE"; return 1 ;;
        esac

        key="$(armos_config_trim "${line%%=*}")"
        value="$(armos_config_trim "${line#*=}")"
        case "$key" in
            [A-Z_]* ) ;;
            *) armos_config_error "$config_file:$line_number: invalid key '$key'"; return 1 ;;
        esac
        armos_config_key_allowed "$key" || {
            armos_config_error "$config_file:$line_number: unknown key '$key'"
            return 1
        }

        if [ "${#value}" -ge 2 ]; then
            quote="${value:0:1}"
            if { [ "$quote" = '"' ] || [ "$quote" = "'" ]; } &&
               [ "${value: -1}" = "$quote" ]; then
                value="${value:1:${#value}-2}"
            fi
        fi

        # An explicitly supplied environment variable has priority over the
        # local configuration file.
        if [ "${!key+x}" != x ]; then
            printf -v "$key" '%s' "$value"
            export "$key"
        fi
    done < "$config_file"

    ARMOS_CONFIG_LOADED=1
    export ARMOS_CONFIG_LOADED
    armos_config_normalize
}

armos_config_validate() {
    local root_dir="$1"
    local platform_dir
    local dimension

    armos_config_normalize || return 1

    case "${KEYBOARD_LAYOUT:-us}" in
        us|us-mac|fr|fr-mac|fr-legacy) ;;
        *)
            armos_config_error \
                "KEYBOARD_LAYOUT expects us, us-mac, fr, fr-mac, or fr-legacy"
            return 1
            ;;
    esac

    if [ -n "${TARGET_ARCH:-}" ] && [ -n "${TARGET_PLATFORM:-}" ]; then
        platform_dir="${TARGET_PLATFORM//-/_}"
        [ -f "$root_dir/arch/$TARGET_ARCH/platform/$platform_dir/platform.mk" ] || {
            armos_config_error "unsupported target $TARGET_ARCH/$TARGET_PLATFORM"
            return 1
        }
    fi

    if [ -n "${SMP_CPUS:-}" ]; then
        case "$SMP_CPUS" in
            *[!0-9]*|'') armos_config_error "SMP_CPUS must be an integer"; return 1 ;;
        esac
        [ "$SMP_CPUS" -ge 1 ] && [ "$SMP_CPUS" -le 64 ] || {
            armos_config_error "SMP_CPUS must be between 1 and 64"
            return 1
        }
    fi

    if [ "${ENABLE_GPU:-0}" = 1 ] &&
       [ "${TARGET_PLATFORM:-qemu-virt}" != qemu-virt ]; then
        armos_config_error "ENABLE_GPU is a QEMU launch option and requires qemu-virt"
        return 1
    fi

    if [ "${ENABLE_WIFI:-0}" = 1 ] &&
       [ "${TARGET_PLATFORM:-qemu-virt}" != raspi3 ]; then
        armos_config_error "ENABLE_WIFI requires TARGET_PLATFORM=raspi3"
        return 1
    fi

    if [ "${ENABLE_HDMI:-0}" = 1 ]; then
        case "${TARGET_PLATFORM:-qemu-virt}" in
            raspi2|raspi3) ;;
            *)
                armos_config_error \
                    "ENABLE_HDMI requires TARGET_PLATFORM=raspi2 or raspi3"
                return 1
                ;;
        esac
    fi

    if [ "${ENABLE_USB:-0}" = 1 ]; then
        case "${TARGET_PLATFORM:-qemu-virt}" in
            raspi2|raspi3) ;;
            *)
                armos_config_error \
                    "ENABLE_USB requires TARGET_PLATFORM=raspi2 or raspi3"
                return 1
                ;;
        esac
    fi

    for dimension in HDMI_WIDTH HDMI_HEIGHT; do
        [ -n "${!dimension:-}" ] || continue
        case "${!dimension}" in
            *[!0-9]*|'') armos_config_error "$dimension must be an integer"; return 1 ;;
        esac
        [ "${!dimension}" -ge 1 ] && [ "${!dimension}" -le 8192 ] || {
            armos_config_error "$dimension must be between 1 and 8192"
            return 1
        }
    done

    if [ "${BUILD_NANO:-0}" = 1 ] && [ "${BUILD_NCURSES:-0}" = 0 ] &&
       [ ! -f "$root_dir/build/${TARGET_ARCH:-arm32}/${TARGET_PLATFORM:-qemu-virt}/userfs/opt/ncurses/lib/libncurses.a" ]; then
        armos_config_error "BUILD_NANO=yes requires BUILD_NCURSES=yes or an installed ncurses bundle"
        return 1
    fi
}

armos_config_show() {
    local key value source

    if [ -f "$ARMOS_CONFIG" ]; then
        source="$ARMOS_CONFIG"
    else
        source="defaults and environment (no local armos.conf)"
    fi
    printf 'ArmOS configuration: %s\n' "$source"
    for key in $ARMOS_CONFIG_KEYS; do
        if [ "${!key+x}" = x ]; then
            value="${!key}"
            if armos_config_is_boolean "$key"; then
                [ "$value" = 1 ] && value=yes || value=no
            fi
            printf '  %-20s %s\n' "$key" "$value"
        fi
    done
}

if [ "${ARMOS_CONFIG_LOADED:-0}" != 1 ]; then
    if [ -n "${ROOT_DIR:-}" ]; then
        _armos_config_root="$ROOT_DIR"
    else
        _armos_config_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    fi
    _armos_config_status=0
    armos_config_load "$_armos_config_root" || _armos_config_status=$?
    unset _armos_config_root
    if [ "$_armos_config_status" -ne 0 ]; then
        if [ "${BASH_SOURCE[0]}" = "$0" ]; then
            exit "$_armos_config_status"
        fi
        return "$_armos_config_status"
    fi
    unset _armos_config_status
fi

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
    TARGET_ARCH="${TARGET_ARCH:-arm32}"
    TARGET_PLATFORM="${TARGET_PLATFORM:-qemu-virt}"
    KEYBOARD_LAYOUT="${KEYBOARD_LAYOUT:-us}"
    ENABLE_NET="${ENABLE_NET:-0}"
    ENABLE_WIFI="${ENABLE_WIFI:-0}"
    ENABLE_GPU="${ENABLE_GPU:-0}"
    ENABLE_HDMI="${ENABLE_HDMI:-0}"
    ENABLE_ILI9341="${ENABLE_ILI9341:-0}"
    ENABLE_USB="${ENABLE_USB:-0}"
    SMP_CPUS="${SMP_CPUS:-1}"
    BUILD_NEWLIB="${BUILD_NEWLIB:-1}"
    BUILD_ALL_USERLAND="${BUILD_ALL_USERLAND:-0}"
    BUILD_TCC="${BUILD_TCC:-1}"
    BUILD_BSD="${BUILD_BSD:-0}"
    BUILD_NCURSES="${BUILD_NCURSES:-0}"
    BUILD_NANO="${BUILD_NANO:-0}"
    BUILD_EPOLL_SHIM="${BUILD_EPOLL_SHIM:-0}"
    BUILD_PIXMAN="${BUILD_PIXMAN:-0}"
    BUILD_TLLIST="${BUILD_TLLIST:-0}"
    BUILD_UTF8PROC="${BUILD_UTF8PROC:-0}"
    BUILD_FREETYPE="${BUILD_FREETYPE:-0}"
    BUILD_EXPAT="${BUILD_EXPAT:-0}"
    BUILD_FONTCONFIG="${BUILD_FONTCONFIG:-0}"
    BUILD_HARFBUZZ="${BUILD_HARFBUZZ:-0}"
    BUILD_FCFT="${BUILD_FCFT:-0}"
    BUILD_FOOT="${BUILD_FOOT:-0}"
    BUILD_NUKLEAR="${BUILD_NUKLEAR:-0}"
    BUILD_XV_DEPS="${BUILD_XV_DEPS:-0}"
    BUILD_FBVIEW="${BUILD_FBVIEW:-0}"
    armos_config_validate "$ROOT_DIR" || exit $?
    case "${1:---show}" in
        --show) armos_config_show ;;
        *) armos_config_error "usage: tools/armos_config.sh [--show]"; exit 2 ;;
    esac
fi
