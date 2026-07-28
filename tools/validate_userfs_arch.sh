#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/validate_userfs_arch.sh
# Layer: Host tooling / userfs validation
#
# Responsibilities:
# - Inspect every ELF file installed in a target ArmOS userfs.
# - Reject binaries for an architecture other than the requested target.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-aarch64-elf-}"
ALLOW_EMPTY=0
QUIET=0
USERFS_ROOT="${USERFS_ROOT:-}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --allow-empty)
            ALLOW_EMPTY=1
            ;;
        --quiet)
            QUIET=1
            ;;
        --root)
            shift
            if [ "$#" -eq 0 ]; then
                echo "error: --root requires a directory" >&2
                exit 2
            fi
            USERFS_ROOT="$1"
            ;;
        *)
            echo "usage: $0 [--root DIR] [--allow-empty] [--quiet]" >&2
            exit 2
            ;;
    esac
    shift
done

# shellcheck source=tools/cross_target_env.sh
source "$ROOT_DIR/tools/cross_target_env.sh"

case "$TARGET_ARCH" in
    arm32)
        EXPECTED_CLASS="ELF32"
        EXPECTED_MACHINE="ARM"
        ;;
    arm64)
        EXPECTED_CLASS="ELF64"
        EXPECTED_MACHINE="AArch64"
        ;;
esac

checked=0
failed=0
while IFS= read -r file; do
    magic="$(LC_ALL=C od -An -tx1 -N4 "$file" 2>/dev/null | tr -d ' \n')"
    [ "$magic" = "7f454c46" ] || continue
    checked=$((checked + 1))
    header="$(${ARCH}readelf -h "$file" 2>/dev/null || true)"
    if ! grep -q "Class:.*$EXPECTED_CLASS" <<<"$header" ||
       ! grep -q "Machine:.*$EXPECTED_MACHINE" <<<"$header"; then
        if [ "$QUIET" -ne 1 ]; then
            echo "error: wrong ELF architecture in userfs: ${file#"$ROOT_DIR/"}" >&2
        fi
        failed=$((failed + 1))
    fi
done < <(find "$USERFS_ROOT" -path "$USERFS_ROOT/legacy" -prune -o -type f -print | sort)

if [ "$checked" -eq 0 ]; then
    if [ "$ALLOW_EMPTY" -eq 1 ]; then
        exit 0
    fi
    if [ "$QUIET" -ne 1 ]; then
        echo "error: userfs contains no ELF files" >&2
    fi
    exit 1
fi
if [ "$failed" -ne 0 ]; then
    if [ "$QUIET" -ne 1 ]; then
        echo "error: $failed of $checked userfs ELF files mismatch $TARGET_ARCH" >&2
    fi
    exit 1
fi

if [ "$QUIET" -ne 1 ]; then
    echo "userfs: $checked ELF files validated as $EXPECTED_CLASS/$EXPECTED_MACHINE"
fi
