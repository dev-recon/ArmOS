#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/with_build_lock.sh
# Layer: Host tooling / cross-build coordination
#
# Responsibilities:
# - Serialize build steps that write one target staging hierarchy.
# - Allow nested ArmOS build scripts to reuse the owning lock.
# - Recover a lock whose owning process no longer exists.
#
# Notes:
# - Callers select build/<arch>/<platform> through ARMOS_BUILD_LOCK_DIR.
# - Different targets remain fully concurrent.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOCK_DIR="${ARMOS_BUILD_LOCK_DIR:-$ROOT_DIR/build/.cross-build.lock}"

if [ "$#" -eq 0 ]; then
    echo "usage: tools/with_build_lock.sh command [argument ...]" >&2
    exit 2
fi

if [ "${ARMOS_BUILD_LOCK_HELD:-0}" = "1" ]; then
    exec "$@"
fi

mkdir -p "$(dirname "$LOCK_DIR")"
waiting=0
while ! mkdir "$LOCK_DIR" 2>/dev/null; do
    owner=""
    if [ -r "$LOCK_DIR/pid" ]; then
        owner="$(sed -n '1p' "$LOCK_DIR/pid" 2>/dev/null || true)"
    fi
    case "$owner" in
        ''|*[!0-9]*)
            ;;
        *)
            if ! kill -0 "$owner" 2>/dev/null; then
                rm -f "$LOCK_DIR/pid"
                rmdir "$LOCK_DIR" 2>/dev/null || true
                continue
            fi
            ;;
    esac
    if [ "$waiting" -eq 0 ]; then
        echo "=== Waiting for ArmOS target build lock ==="
        waiting=1
    fi
    sleep 1
done

printf '%s\n' "$$" > "$LOCK_DIR/pid"

release_build_lock()
{
    rm -f "$LOCK_DIR/pid"
    rmdir "$LOCK_DIR" 2>/dev/null || true
}

trap release_build_lock EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

export ARMOS_BUILD_LOCK_HELD=1
"$@"
