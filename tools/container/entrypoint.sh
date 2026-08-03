#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/container/entrypoint.sh
# Layer: Host tooling / container runtime
#
# Responsibilities:
# - Give root and arbitrary host-mapped users a writable transient HOME.
# - Validate the mounted workspace before starting a build.
# - Keep Git and generated-file ownership predictable across host systems.

set -euo pipefail

if [ ! -f /workspace/build.sh ] || [ ! -d /workspace/kernel ]; then
    echo "error: mount the ArmOS repository at /workspace" >&2
    exit 2
fi

container_home="/tmp/armos-container-${UID:-0}"
mkdir -p "$container_home"
export HOME="$container_home"
export ARMOS_CONTAINER=1
umask 022

git config --global --add safe.directory /workspace

exec "$@"
