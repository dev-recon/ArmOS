#!/usr/bin/env bash
# Shared QEMU selection and optional version enforcement for host scripts.

QEMU_PINNED_VERSION="${QEMU_PINNED_VERSION:-10.0.2}"

select_arm_qemu() {
    local explicit="${1:-}"
    local root_dir="${2:?repository root is required}"
    local target_arch="${3:-arm32}"
    local qemu_name="qemu-system-arm"
    local pinned_qemu
    local host_tools_qemu=""
    local host_id=""

    if [ "$target_arch" = "arm64" ]; then
        qemu_name="qemu-system-aarch64"
    fi
    pinned_qemu="$root_dir/build/qemu-$QEMU_PINNED_VERSION/install/bin/$qemu_name"

    if [ "$(uname -s)" = "Linux" ]; then
        case "$(uname -m)" in
            x86_64|amd64) host_id="linux-amd64" ;;
            aarch64|arm64) host_id="linux-arm64" ;;
        esac
        if [ -n "$host_id" ]; then
            host_tools_qemu="$root_dir/build/host-tools/qemu/$host_id/qemu-$QEMU_PINNED_VERSION/install/bin/$qemu_name"
        fi
    fi

    if [ -n "$explicit" ]; then
        printf '%s\n' "$explicit"
    elif [ -n "${QEMU:-}" ]; then
        printf '%s\n' "$QEMU"
    elif [ -x "$pinned_qemu" ]; then
        printf '%s\n' "$pinned_qemu"
    elif [ -n "$host_tools_qemu" ] && [ -x "$host_tools_qemu" ]; then
        printf '%s\n' "$host_tools_qemu"
    elif [ -x "/opt/homebrew/bin/$qemu_name" ]; then
        printf '%s\n' "/opt/homebrew/bin/$qemu_name"
    elif [ -x "/usr/local/bin/$qemu_name" ]; then
        printf '%s\n' "/usr/local/bin/$qemu_name"
    else
        printf '%s\n' "$qemu_name"
    fi
}

find_qemu_data_dir() {
    local qemu_binary="${1:?QEMU binary is required}"
    local resolved_binary
    local candidate

    resolved_binary="$(command -v "$qemu_binary" 2>/dev/null || true)"
    [ -n "$resolved_binary" ] || return 0

    candidate="$(cd "$(dirname "$resolved_binary")/../share/qemu" 2>/dev/null && pwd || true)"
    if [ -n "$candidate" ] && [ -f "$candidate/efi-virtio.rom" ]; then
        printf '%s\n' "$candidate"
    fi
}

require_qemu_version() {
    local qemu_binary="${1:?QEMU binary is required}"
    local required="${QEMU_REQUIRED_VERSION:-}"
    local detected
    local version_line

    [ -n "$required" ] || return 0
    version_line="$("$qemu_binary" --version | head -n 1)"
    detected="$(printf '%s\n' "$version_line" | sed -n 's/^QEMU emulator version \([^ ]*\).*/\1/p')"
    if [ "$detected" != "$required" ]; then
        echo "Error: QEMU $required is required, found: $version_line" >&2
        echo "Run ./tools/build_qemu_10_0_2.sh or unset QEMU_REQUIRED_VERSION." >&2
        return 1
    fi
}

# Measure the terminal which owns QEMU's stdio UART.  A PL011 UART transports
# bytes only, so the guest cannot discover this geometry by itself.  QEMU's
# direct-kernel boot path can, however, carry it in /chosen/bootargs.
qemu_host_tty_geometry() {
    local measured=""
    local rows="${ARMOS_TTY_ROWS:-}"
    local cols="${ARMOS_TTY_COLS:-}"

    if [ -z "$rows" ] || [ -z "$cols" ]; then
        if [ -r /dev/tty ]; then
            measured="$(stty size </dev/tty 2>/dev/null || true)"
            if [ -n "$measured" ]; then
                read -r rows cols <<<"$measured"
            fi
        fi
    fi

    if ! [[ "$rows" =~ ^[0-9]+$ && "$cols" =~ ^[0-9]+$ ]] ||
       [ "$rows" -lt 2 ] || [ "$rows" -gt 1000 ] ||
       [ "$cols" -lt 2 ] || [ "$cols" -gt 1000 ]; then
        return 1
    fi

    QEMU_HOST_TTY_ROWS="$rows"
    QEMU_HOST_TTY_COLS="$cols"
    return 0
}

qemu_console_bootargs() {
    local args="${QEMU_KERNEL_CMDLINE:-}"

    if qemu_host_tty_geometry; then
        args="${args:+$args }armos.tty0.rows=$QEMU_HOST_TTY_ROWS armos.tty0.cols=$QEMU_HOST_TTY_COLS"
    fi
    printf '%s\n' "$args"
}
