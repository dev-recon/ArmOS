#!/usr/bin/env bash
#
# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/bootstrap_mesa_host_tools.sh
# Layer: Host tooling / Reproducible Mesa prerequisites
#
# Responsibilities:
# - Build pinned GNU M4 and Bison releases for the current build host.
# - Provision a pinned Python environment for Meson and Mesa generators.
# - Verify every downloaded archive before extraction.
# - Keep host executables outside architecture/platform target directories.
#
# Notes:
# - These tools execute on the host and are never installed in ArmOS userfs.
# - Use --print-prefix from another build script to obtain the clean prefix.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
M4_VERSION="1.4.19"
M4_SHA256="63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96"
M4_URL="https://ftp.gnu.org/gnu/m4/m4-$M4_VERSION.tar.xz"
BISON_VERSION="3.8.2"
BISON_SHA256="9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2"
BISON_URL="https://ftp.gnu.org/gnu/bison/bison-$BISON_VERSION.tar.xz"
MESON_VERSION="1.8.2"
NINJA_VERSION="1.11.1.4"
MAKO_VERSION="1.3.10"
MARKUPSAFE_VERSION="3.0.2"
PYYAML_VERSION="6.0.2"
PACKAGING_VERSION="24.2"
HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_CPU="$(uname -m | tr -d '\n' | tr -c '[:alnum:]_.-' '_')"
HOST_ID="$HOST_OS-$HOST_CPU"
WORK_ROOT="${WORK_ROOT:-$ROOT_DIR/build/host-tools}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$ROOT_DIR/build/downloads/host-tools}"
HOST_WORK="$WORK_ROOT/$HOST_ID"
PREFIX="${PREFIX:-$HOST_WORK/install}"
SOURCE_ROOT="$HOST_WORK/source"
BUILD_ROOT="$HOST_WORK/build"
STAMP="$HOST_WORK/.armos-mesa-host-tools"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
PYTHON_PREFIX="$PREFIX/python"
PYTHON_BIN="$PYTHON_PREFIX/bin/python3"
PRINT_MODE=""
HOST_PYTHON_CANDIDATE="$(command -v python3 || true)"
HOST_PYTHON=""

if [ -n "$HOST_PYTHON_CANDIDATE" ]; then
    HOST_PYTHON="$(env -u __PYVENV_LAUNCHER__ -u VIRTUAL_ENV \
        "$HOST_PYTHON_CANDIDATE" -c '
import os
import sys
print(os.path.realpath(getattr(sys, "_base_executable", sys.executable)))
' 2>/dev/null || true)"
fi

if [ "${1:-}" = "--print-prefix" ]; then
    PRINT_MODE="prefix"
    shift
elif [ "${1:-}" = "--print-python" ]; then
    PRINT_MODE="python"
    shift
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--print-prefix|--print-python]" >&2
    exit 2
fi

log()
{
    if [ -n "$PRINT_MODE" ]; then
        printf '%s\n' "$*" >&2
    else
        printf '%s\n' "$*"
    fi
}

run_noisy()
{
    if [ -n "$PRINT_MODE" ]; then
        "$@" >&2
    else
        "$@"
    fi
}

build_m4()
{
    (
        cd "$BUILD_ROOT/m4-$M4_VERSION"
        "$SOURCE_ROOT/m4-$M4_VERSION/configure" \
            --prefix="$PREFIX" \
            --disable-dependency-tracking \
            --disable-nls
        make -j"$JOBS" MAKEINFO=true
        make install MAKEINFO=true
    )
}

build_bison()
{
    (
        cd "$BUILD_ROOT/bison-$BISON_VERSION"
        PATH="$PREFIX/bin:$PATH" M4="$PREFIX/bin/m4" \
            "$SOURCE_ROOT/bison-$BISON_VERSION/configure" \
            --prefix="$PREFIX" \
            --disable-dependency-tracking \
            --disable-nls
        PATH="$PREFIX/bin:$PATH" make -j"$JOBS" MAKEINFO=true
        PATH="$PREFIX/bin:$PATH" make install MAKEINFO=true
    )
}

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "error: sha256sum or shasum is required" >&2
        return 1
    fi
}

fetch_archive()
{
    local name="$1"
    local url="$2"
    local expected="$3"
    local archive="$DOWNLOAD_DIR/$name"
    local actual

    if [ ! -f "$archive" ]; then
        log "=== Downloading $name ===" >&2
        if ! curl -L --fail --silent --show-error "$url" -o "$archive.tmp"; then
            rm -f "$archive.tmp"
            return 1
        fi
        mv "$archive.tmp" "$archive"
    fi

    actual="$(sha256_file "$archive")"
    if [ "$actual" != "$expected" ]; then
        echo "error: SHA-256 mismatch for $archive" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        return 1
    fi
    printf '%s\n' "$archive"
}

version_is()
{
    local executable="$1"
    local expected="$2"

    [ -x "$executable" ] &&
        "$executable" --version 2>/dev/null | head -n 1 |
        grep -Fq "$expected"
}

python_environment_is_ready()
{
    [ -x "$PYTHON_BIN" ] && [ -f "$PYTHON_PREFIX/bin/meson" ] &&
        env -u __PYVENV_LAUNCHER__ -u VIRTUAL_ENV \
        "$PYTHON_BIN" "$PYTHON_PREFIX/bin/meson" --version 2>/dev/null |
        grep -Fxq "$MESON_VERSION" &&
        env -u __PYVENV_LAUNCHER__ -u VIRTUAL_ENV "$PYTHON_BIN" -c '
from importlib.metadata import version
expected = {
    "meson": "'"$MESON_VERSION"'",
    "ninja": "'"$NINJA_VERSION"'",
    "Mako": "'"$MAKO_VERSION"'",
    "MarkupSafe": "'"$MARKUPSAFE_VERSION"'",
    "PyYAML": "'"$PYYAML_VERSION"'",
    "packaging": "'"$PACKAGING_VERSION"'",
}
raise SystemExit(0 if all(version(name) == wanted
                          for name, wanted in expected.items()) else 1)
' >/dev/null 2>&1
}

print_result()
{
    case "$PRINT_MODE" in
        prefix)
            printf '%s\n' "$PREFIX"
            ;;
        python)
            printf '%s\n' "$PYTHON_BIN"
            ;;
        *)
            printf 'M4:     %s\nBison:  %s\nMeson:  %s\nNinja:  %s\nPython: %s\n' \
                "$PREFIX/bin/m4" "$PREFIX/bin/bison" \
                "$PYTHON_PREFIX/bin/meson" "$PYTHON_PREFIX/bin/ninja" \
                "$PYTHON_BIN"
            ;;
    esac
}

for tool in cc curl make tar xz awk grep; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required bootstrap tool '$tool' not found" >&2
        exit 1
    fi
done
if [ -z "$HOST_PYTHON" ] || [ ! -x "$HOST_PYTHON" ]; then
    echo "error: a usable base Python 3 interpreter is required" >&2
    exit 1
fi

mkdir -p "$DOWNLOAD_DIR" "$SOURCE_ROOT" "$BUILD_ROOT" "$HOST_WORK"

CONTRACT="m4=$M4_VERSION:$M4_SHA256 bison=$BISON_VERSION:$BISON_SHA256 meson=$MESON_VERSION ninja=$NINJA_VERSION mako=$MAKO_VERSION markupsafe=$MARKUPSAFE_VERSION pyyaml=$PYYAML_VERSION packaging=$PACKAGING_VERSION host=$HOST_ID python=$("$HOST_PYTHON" --version 2>&1) cc=$(cc --version 2>/dev/null | head -n 1)"
if [ "${ARMOS_FORCE_HOST_TOOLS_REBUILD:-0}" != "1" ] &&
   [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$CONTRACT" ] &&
   version_is "$PREFIX/bin/m4" "m4 (GNU M4) $M4_VERSION" &&
   version_is "$PREFIX/bin/bison" "bison (GNU Bison) $BISON_VERSION" &&
   python_environment_is_ready; then
    log "=== Mesa host tools: reusing $HOST_ID ==="
    print_result
    exit 0
fi

if [ "${ARMOS_FORCE_HOST_TOOLS_REBUILD:-0}" = "1" ] ||
   ! version_is "$PREFIX/bin/m4" "m4 (GNU M4) $M4_VERSION"; then
    M4_ARCHIVE="$(fetch_archive "m4-$M4_VERSION.tar.xz" "$M4_URL" "$M4_SHA256")"
    rm -rf "$SOURCE_ROOT/m4-$M4_VERSION" "$BUILD_ROOT/m4-$M4_VERSION"
    mkdir -p "$SOURCE_ROOT/m4-$M4_VERSION" "$BUILD_ROOT/m4-$M4_VERSION"
    tar -xJf "$M4_ARCHIVE" -C "$SOURCE_ROOT/m4-$M4_VERSION" --strip-components=1

    log "=== Building GNU M4 $M4_VERSION for $HOST_ID ==="
    run_noisy build_m4
fi

if [ "${ARMOS_FORCE_HOST_TOOLS_REBUILD:-0}" = "1" ] ||
   ! version_is "$PREFIX/bin/bison" "bison (GNU Bison) $BISON_VERSION"; then
    BISON_ARCHIVE="$(fetch_archive "bison-$BISON_VERSION.tar.xz" "$BISON_URL" "$BISON_SHA256")"
    rm -rf "$SOURCE_ROOT/bison-$BISON_VERSION" "$BUILD_ROOT/bison-$BISON_VERSION"
    mkdir -p "$SOURCE_ROOT/bison-$BISON_VERSION" "$BUILD_ROOT/bison-$BISON_VERSION"
    tar -xJf "$BISON_ARCHIVE" -C "$SOURCE_ROOT/bison-$BISON_VERSION" --strip-components=1

    log "=== Building GNU Bison $BISON_VERSION for $HOST_ID ==="
    run_noisy build_bison
fi

if ! version_is "$PREFIX/bin/m4" "m4 (GNU M4) $M4_VERSION"; then
    echo "error: installed M4 failed version validation" >&2
    exit 1
fi
if ! version_is "$PREFIX/bin/bison" "bison (GNU Bison) $BISON_VERSION"; then
    echo "error: installed Bison failed version validation" >&2
    exit 1
fi

if [ "${ARMOS_FORCE_HOST_TOOLS_REBUILD:-0}" = "1" ] ||
   ! python_environment_is_ready; then
    log "=== Provisioning pinned Mesa Python tools for $HOST_ID ==="
    rm -rf "$PYTHON_PREFIX"
    run_noisy env -u __PYVENV_LAUNCHER__ -u VIRTUAL_ENV \
        "$HOST_PYTHON" -m venv "$PYTHON_PREFIX"
    run_noisy env -u __PYVENV_LAUNCHER__ -u VIRTUAL_ENV \
        PIP_DISABLE_PIP_VERSION_CHECK=1 \
        "$PYTHON_BIN" -m pip install \
        --no-compile \
        "meson==$MESON_VERSION" \
        "ninja==$NINJA_VERSION" \
        "Mako==$MAKO_VERSION" \
        "MarkupSafe==$MARKUPSAFE_VERSION" \
        "PyYAML==$PYYAML_VERSION" \
        "packaging==$PACKAGING_VERSION"
fi

if ! python_environment_is_ready; then
    echo "error: installed Mesa Python environment failed validation" >&2
    exit 1
fi

printf '%s\n' "$CONTRACT" > "$STAMP"
log "=== Mesa host tools ready for $HOST_ID ==="
print_result
