#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/check_release_image_hygiene.sh
# Layer: Host tooling / release validation
#
# Responsibilities:
# - Reject release artifacts that expose host build paths or usernames.
# - Reject macOS metadata and non-example Wi-Fi credentials.
#
# Notes:
# - The scan operates on printable strings, so it also covers files embedded
#   in raw disk and filesystem images without mounting them.

set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: tools/check_release_image_hygiene.sh FILE_OR_DIRECTORY [...]" >&2
    exit 2
fi

for tool in find grep gzip mktemp rm strings tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required tool '$tool' not found" >&2
        exit 1
    }
done

HOST_PATH_PATTERN='/Users/|/private/var/folders/|/opt/homebrew/|/usr/local/(Cellar|opt)/|/home/[^/]+/(dev|src|work|workspace|projects?)/|/root/(dev|src|work|workspace|projects?)/'
# Raw binaries can legitimately contain short printable instruction/data
# fragments such as "._%c".  Restrict the heuristic used for opaque images to
# portable AppleDouble filenames; directory and archive scans still inspect
# actual names and reject every ._* entry without relying on this pattern.
MAC_METADATA_PATTERN='(^|/)(\.DS_Store|\._[[:alnum:]][[:alnum:]._-]*|\.Spotlight-V100|\.Trashes)($|/)'
failures=0
scan_serial=0
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/armos-release-hygiene.XXXXXX")"
cleanup()
{
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

scan_file()
{
    local path="$1"
    local printable
    local host_paths
    local metadata
    local credentials

    scan_serial=$((scan_serial + 1))
    printable="$WORK_DIR/strings-$scan_serial.txt"
    strings -a "$path" > "$printable"

    host_paths="$(LC_ALL=C grep -E -m 20 "$HOST_PATH_PATTERN" \
        "$printable" || true)"
    if [ -n "$host_paths" ]; then
        echo "error: host build information found in $path:" >&2
        printf '%s\n' "$host_paths" >&2
        failures=$((failures + 1))
    fi

    metadata="$(LC_ALL=C grep -E -m 20 "$MAC_METADATA_PATTERN" \
        "$printable" || true)"
    if [ -n "$metadata" ]; then
        echo "error: host filesystem metadata found in $path:" >&2
        printf '%s\n' "$metadata" >&2
        failures=$((failures + 1))
    fi

    credentials="$(LC_ALL=C grep -E '^WIFI_PASSWORD=' "$printable" |
        LC_ALL=C grep -Ev '^WIFI_PASSWORD=(replace-me)?$' || true)"
    if [ -n "$credentials" ]; then
        echo "error: runtime Wi-Fi credentials found in $path" >&2
        failures=$((failures + 1))
    fi

    rm -f "$printable"
}

scan_archive()
{
    local path="$1"
    local manifest="$WORK_DIR/archive-list.txt"
    local verbose="$WORK_DIR/archive-verbose.txt"
    local expanded="$WORK_DIR/archive.tar"
    local metadata
    local owners

    if ! tar -tzf "$path" > "$manifest"; then
        echo "error: invalid release archive: $path" >&2
        failures=$((failures + 1))
        return
    fi
    tar -tvzf "$path" > "$verbose"

    metadata="$(LC_ALL=C grep -E -m 20 "$MAC_METADATA_PATTERN" \
        "$manifest" || true)"
    if [ -n "$metadata" ]; then
        echo "error: host filesystem metadata found in $path:" >&2
        printf '%s\n' "$metadata" >&2
        failures=$((failures + 1))
    fi

    owners="$(LC_ALL=C grep -Ev \
        '[[:space:]]root([[:space:]]+|/)root[[:space:]]' \
        "$verbose" || true)"
    if [ -n "$owners" ]; then
        echo "error: non-root archive ownership found in $path:" >&2
        printf '%s\n' "$owners" >&2
        failures=$((failures + 1))
    fi

    gzip -cd "$path" > "$expanded"
    scan_file "$expanded"
    rm -f "$manifest" "$verbose" "$expanded"
}

scan_directory()
{
    local input="$1"
    local metadata
    local file

    while IFS= read -r metadata; do
        echo "error: host filesystem metadata found: $metadata" >&2
        failures=$((failures + 1))
    done < <(find "$input" \( \
        -name '.DS_Store' -o -name '._*' -o \
        -name '.Spotlight-V100' -o -name '.Trashes' \
    \) -print)
    while IFS= read -r file; do
        case "$file" in
            *.tar.gz|*.tgz) scan_archive "$file" ;;
            *) scan_file "$file" ;;
        esac
    done < <(find "$input" -type f -print)
}

for input in "$@"; do
    if [ ! -e "$input" ]; then
        echo "error: release artifact does not exist: $input" >&2
        failures=$((failures + 1))
        continue
    fi
    if [ -d "$input" ]; then
        scan_directory "$input"
    else
        case "$input" in
            *.tar.gz|*.tgz) scan_archive "$input" ;;
            *) scan_file "$input" ;;
        esac
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "error: release hygiene audit failed with $failures finding(s)" >&2
    exit 1
fi

echo "Release hygiene audit passed"
