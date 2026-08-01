#!/usr/bin/env bash
# Build the exact ArmOS reference QEMU release in a repo-local prefix.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_VERSION="10.0.2"
QEMU_URL="https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"
QEMU_SHA256="ef786f2398cb5184600f69aef4d5d691efd44576a3cff4126d38d4c6fec87759"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/build/qemu-$QEMU_VERSION}"
DOWNLOAD_DIR="$WORK_DIR/download"
ARCHIVE="$DOWNLOAD_DIR/qemu-$QEMU_VERSION.tar.xz"
SOURCE_DIR="$WORK_DIR/src"
BUILD_DIR="$WORK_DIR/build"
PREFIX="${PREFIX:-$WORK_DIR/install}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
PATCH_DIR="$ROOT_DIR/tools/patches/qemu-$QEMU_VERSION"
INSTALL_DEPS=0

usage() {
    cat <<'EOF'
Usage: ./tools/build_qemu_10_0_2.sh [--install-deps]

  --install-deps  Install missing SDL/OpenGL/VirGL host prerequisites first.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$INSTALL_DEPS" -eq 1 ]; then
    "$ROOT_DIR/tools/bootstrap_qemu_10_0_2_host_deps.sh" --install
else
    "$ROOT_DIR/tools/bootstrap_qemu_10_0_2_host_deps.sh" --check
fi

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "error: sha256sum or shasum is required" >&2
        return 1
    fi
}

for tool in curl tar make pkg-config python3 ninja git grep patch; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required host tool '$tool' not found" >&2
        exit 1
    fi
done

QEMU_CONFIGURE_ARGS=(
    "--prefix=$PREFIX"
    --target-list=arm-softmmu,aarch64-softmmu
    --disable-docs
    --disable-werror
    --enable-sdl
    --enable-opengl
    --enable-virglrenderer
    --disable-nettle
    --disable-spice
    --disable-spice-protocol
)
QEMU_REQUIRED_DISPLAY_BACKENDS=(sdl)
if [ "$(uname -s)" = "Linux" ]; then
    QEMU_CONFIGURE_ARGS+=(--disable-gtk)
elif [ "$(uname -s)" = "Darwin" ]; then
    QEMU_CONFIGURE_ARGS+=(--enable-cocoa)
    QEMU_REQUIRED_DISPLAY_BACKENDS+=(cocoa)
fi

mkdir -p "$DOWNLOAD_DIR" "$WORK_DIR"
if [ ! -f "$ARCHIVE" ]; then
    echo "=== Downloading QEMU $QEMU_VERSION ==="
    curl -L --fail "$QEMU_URL" -o "$ARCHIVE.tmp"
    mv "$ARCHIVE.tmp" "$ARCHIVE"
fi

actual_sha256="$(sha256_file "$ARCHIVE")"
if [ "$actual_sha256" != "$QEMU_SHA256" ]; then
    echo "error: QEMU archive SHA-256 mismatch" >&2
    echo "expected: $QEMU_SHA256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
fi
echo "QEMU archive SHA-256 verified."

if [ ! -x "$SOURCE_DIR/configure" ]; then
    echo "=== Extracting QEMU $QEMU_VERSION ==="
    rm -rf "$SOURCE_DIR"
    mkdir -p "$SOURCE_DIR"
    tar -xJf "$ARCHIVE" -C "$SOURCE_DIR" --strip-components=1
fi

for patch_file in "$PATCH_DIR"/*.patch; do
    [ -e "$patch_file" ] || continue
    if patch --dry-run -d "$SOURCE_DIR" -p1 < "$patch_file" >/dev/null; then
        echo "=== Applying $(basename "$patch_file") ==="
        patch -d "$SOURCE_DIR" -p1 < "$patch_file"
    elif patch --dry-run -R -d "$SOURCE_DIR" -p1 < "$patch_file" >/dev/null; then
        echo "Patch already applied: $(basename "$patch_file")"
    else
        echo "error: patch does not apply cleanly: $patch_file" >&2
        exit 1
    fi
done

mkdir -p "$BUILD_DIR" "$PREFIX"
cd "$BUILD_DIR"

echo "=== Configuring QEMU $QEMU_VERSION (ARM32 + ARM64 system emulation) ==="
"$SOURCE_DIR/configure" "${QEMU_CONFIGURE_ARGS[@]}"

echo "=== Building QEMU $QEMU_VERSION ==="
make -j"$JOBS"
make install

for qemu_name in qemu-system-arm qemu-system-aarch64; do
    QEMU_BINARY="$PREFIX/bin/$qemu_name"
    version_line="$("$QEMU_BINARY" --version | head -n 1)"
    detected_version="$(printf '%s\n' "$version_line" | sed -n 's/^QEMU emulator version \([^ ]*\).*/\1/p')"
    if [ "$detected_version" != "$QEMU_VERSION" ]; then
        echo "error: unexpected installed QEMU version: $version_line" >&2
        exit 1
    fi
    QEMU_DISPLAY_HELP="$("$QEMU_BINARY" -display help 2>/dev/null)"
    for display_backend in "${QEMU_REQUIRED_DISPLAY_BACKENDS[@]}"; do
        if ! printf '%s\n' "$QEMU_DISPLAY_HELP" | grep -qx "$display_backend"; then
            echo "error: $QEMU_BINARY lacks the required '$display_backend' display backend" >&2
            exit 1
        fi
    done
    if ! "$QEMU_BINARY" -device help 2>/dev/null |
           grep -q 'name "virtio-gpu-gl-device"'; then
        echo "error: $QEMU_BINARY lacks VirGL support" >&2
        exit 1
    fi
    echo "Installed: $QEMU_BINARY"
    echo "Displays:  ${QEMU_REQUIRED_DISPLAY_BACKENDS[*]}"
done

if [ ! -f "$PREFIX/share/qemu/efi-virtio.rom" ]; then
    echo "error: QEMU firmware resources were not installed in $PREFIX/share/qemu" >&2
    exit 1
fi

echo
echo "$version_line"
echo "ArmOS boot scripts will prefer this repo-local binary automatically."
