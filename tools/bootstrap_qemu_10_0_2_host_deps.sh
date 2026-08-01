#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/bootstrap_qemu_10_0_2_host_deps.sh
# Layer: Host tooling / QEMU 10.0.2 VirGL prerequisites
#
# Responsibilities:
# - Install the supported host packages needed by QEMU's VirGL backend.
# - Validate tools and pkg-config modules before the QEMU source build starts.
# - Keep host dependencies outside architecture/platform target directories.
#
# Notes:
# - Homebrew and apt remain the owners of these host libraries.
# - Nothing installed here enters an ArmOS userfs or target sysroot.

set -euo pipefail

MODE=check

usage()
{
    cat <<'EOF'
Usage: ./tools/bootstrap_qemu_10_0_2_host_deps.sh [--check|--install]

  --check    Validate the host without changing it (default).
  --install  Install missing packages with Homebrew or apt.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --check) MODE=check ;;
        --install) MODE=install ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

install_macos()
{
    local package
    local packages=(
        pkg-config ninja glib pixman sdl2 libepoxy virglrenderer libslirp
    )

    if ! command -v brew >/dev/null 2>&1; then
        echo "error: Homebrew is required to install QEMU host dependencies" >&2
        echo "install it from https://brew.sh, then rerun this command" >&2
        exit 1
    fi

    for package in "${packages[@]}"; do
        if ! brew list --versions "$package" >/dev/null 2>&1; then
            brew install "$package"
        fi
    done
}

install_linux()
{
    local sudo_command=()
    local packages=(
        build-essential curl git patch pkg-config ninja-build python3
        xz-utils libglib2.0-dev libpixman-1-dev libsdl2-dev libepoxy-dev
        libvirglrenderer-dev libslirp-dev zlib1g-dev
    )

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "error: automatic installation currently supports apt-based Linux hosts" >&2
        echo "install the modules listed by --check with your package manager" >&2
        exit 1
    fi
    if [ "$(id -u)" -ne 0 ]; then
        if ! command -v sudo >/dev/null 2>&1; then
            echo "error: sudo is required to install host packages" >&2
            exit 1
        fi
        sudo_command=(sudo)
    fi

    "${sudo_command[@]}" apt-get update
    "${sudo_command[@]}" apt-get install -y "${packages[@]}"
}

case "$(uname -s)" in
    Darwin)
        [ "$MODE" = install ] && install_macos
        ;;
    Linux)
        [ "$MODE" = install ] && install_linux
        ;;
    *)
        echo "error: unsupported QEMU build host: $(uname -s)" >&2
        exit 1
        ;;
esac

missing=0
for tool in awk cc curl git grep make ninja patch pkg-config python3 sed tar xz; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing host tool: $tool" >&2
        missing=1
    fi
done

for module in glib-2.0 pixman-1 sdl2 epoxy virglrenderer slirp; do
    if ! pkg-config --exists "$module" 2>/dev/null; then
        echo "missing pkg-config module: $module" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    if [ "$MODE" = check ]; then
        echo "hint: rerun with --install" >&2
    fi
    exit 1
fi

if [ "$(uname -s)" = Darwin ]; then
    virgl_libdir="$(pkg-config --variable=libdir virglrenderer)"
    virgl_library=""
    for candidate in "$virgl_libdir"/libvirglrenderer.*.dylib \
                     "$virgl_libdir"/libvirglrenderer.dylib; do
        if [ -f "$candidate" ]; then
            virgl_library="$candidate"
            break
        fi
    done
    if [ -z "$virgl_library" ]; then
        echo "error: cannot locate the virglrenderer dynamic library" >&2
        exit 1
    fi
    # Upstream also keeps one adaptive guest-shader path at GLSL 1.30 in
    # vrend_shader.c.  The incompatible internal blitter contributes several
    # additional exact strings; the Apple patch reduces the installed image
    # to the single legitimate occurrence.
    virgl_glsl_130_count="$(strings "$virgl_library" |
        grep -c -x '#version 130' || true)"
    if [ "$virgl_glsl_130_count" -gt 1 ]; then
        echo "error: virglrenderer uses GLSL 1.30 blitter shaders, which macOS core OpenGL rejects" >&2
        echo "apply tools/patches/virglrenderer-1.3.0/0001-macos-core-profile-blitter-glsl.patch" >&2
        echo "to the virglrenderer 1.3.0 source and reinstall it before rebuilding QEMU" >&2
        exit 1
    fi
fi

echo "QEMU 10.0.2 VirGL host prerequisites are available."
printf '  SDL2:          %s\n' "$(pkg-config --modversion sdl2)"
printf '  libepoxy:      %s\n' "$(pkg-config --modversion epoxy)"
printf '  virglrenderer: %s\n' "$(pkg-config --modversion virglrenderer)"
printf '  libslirp:      %s\n' "$(pkg-config --modversion slirp)"
