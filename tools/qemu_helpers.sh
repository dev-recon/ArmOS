#!/usr/bin/env bash
# Shared QEMU selection and optional version enforcement for host scripts.

QEMU_PINNED_VERSION="${QEMU_PINNED_VERSION:-10.0.2}"

select_arm_qemu() {
    local explicit="${1:-}"
    local root_dir="${2:?repository root is required}"
    local target_arch="${3:-arm32}"
    local qemu_name="qemu-system-arm"
    local pinned_qemu

    if [ "$target_arch" = "arm64" ]; then
        qemu_name="qemu-system-aarch64"
    fi
    pinned_qemu="$root_dir/build/qemu-$QEMU_PINNED_VERSION/install/bin/$qemu_name"

    if [ -n "$explicit" ]; then
        printf '%s\n' "$explicit"
    elif [ -n "${QEMU:-}" ]; then
        printf '%s\n' "$QEMU"
    elif [ -x "$pinned_qemu" ]; then
        printf '%s\n' "$pinned_qemu"
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
