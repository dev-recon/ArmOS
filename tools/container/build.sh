#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/container/build.sh
# Layer: Host tooling / container launcher
#
# Responsibilities:
# - Build or select the reproducible ArmOS build container.
# - Mount the checkout without hiding target-local build products.
# - Pass one explicit ArmOS configuration to the ordinary build entry point.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${ARMOS_BUILD_IMAGE:-armos-build:local}"
CONFIG="configs/qemu-virt-arm64.conf"
BUILD_IMAGE=0
BUILD_OPTION=""
BUILD_QEMU=0

usage()
{
    cat <<'EOF'
Usage: tools/container/build.sh [options]

Options:
  --build-image       Force a refresh of the local container image.
  --image NAME        Container image (default: armos-build:local).
  --config FILE       ArmOS configuration relative to the repository.
  --rebuild           Force the complete userland rebuild contract.
  --reconfigure       Force third-party configure steps.
  --qemu              Build only the pinned Linux QEMU/VirGL toolchain.
  -h, --help          Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-image) BUILD_IMAGE=1 ;;
        --qemu) BUILD_QEMU=1 ;;
        --image)
            [ "$#" -ge 2 ] || { echo "error: --image needs a value" >&2; exit 2; }
            IMAGE="$2"
            shift
            ;;
        --config)
            [ "$#" -ge 2 ] || { echo "error: --config needs a value" >&2; exit 2; }
            CONFIG="$2"
            shift
            ;;
        --rebuild|--reconfigure) BUILD_OPTION="$1" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case "$CONFIG" in
    /*|..|../*|*/..|*/../*)
        echo "error: configuration must remain inside the repository" >&2
        exit 2
        ;;
esac
if [ ! -f "$ROOT_DIR/$CONFIG" ]; then
    echo "error: ArmOS configuration not found: $CONFIG" >&2
    exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "error: Docker is not installed or not available in PATH" >&2
    exit 1
fi

docker_command=(docker)
if ! docker info >/dev/null 2>&1; then
    if [ "$(uname -s)" = "Linux" ] && command -v sudo >/dev/null 2>&1; then
        echo "=== Docker is not accessible as $(id -un); trying through sudo ==="
        if sudo docker info >/dev/null; then
            docker_command=(sudo docker)
        else
            echo "error: the Docker daemon is unavailable, even through sudo" >&2
            echo "start Docker, or grant the current user access to its socket" >&2
            exit 1
        fi
    else
        echo "error: the Docker daemon is unavailable or access is denied" >&2
        echo "start Docker and grant the current user access to its socket" >&2
        exit 1
    fi
fi

if [ "$BUILD_IMAGE" = "1" ] ||
   ! "${docker_command[@]}" image inspect "$IMAGE" >/dev/null 2>&1; then
    if [ "$BUILD_IMAGE" != "1" ]; then
        echo "=== Container image $IMAGE not found; building it automatically ==="
    fi
    "${docker_command[@]}" build --pull \
        --file "$ROOT_DIR/tools/container/Dockerfile" \
        --tag "$IMAGE" \
        "$ROOT_DIR"
fi

docker_args=(
    run --rm --init
    --mount "type=bind,source=$ROOT_DIR,target=/workspace"
    --workdir /workspace
    --env "ARMOS_CONFIG=$CONFIG"
)
if [ "$(uname -s)" = "Linux" ]; then
    docker_args+=(--user "$(id -u):$(id -g)")
fi
if [ "$BUILD_QEMU" = "1" ]; then
    if [ -n "$BUILD_OPTION" ]; then
        echo "error: --qemu cannot be combined with $BUILD_OPTION" >&2
        exit 2
    fi
    if [ "$(uname -s)" != "Linux" ]; then
        echo "=== Note: --qemu produces a Linux QEMU for container/WSL testing ==="
        echo "A native host graphics window requires tools/build_qemu_10_0_2.sh."
    fi
    docker_args+=("$IMAGE" ./tools/container/build-qemu.sh)
else
    docker_args+=("$IMAGE" ./build.sh)
    if [ -n "$BUILD_OPTION" ]; then
        docker_args+=("$BUILD_OPTION")
    fi
fi

exec "${docker_command[@]}" "${docker_args[@]}"
