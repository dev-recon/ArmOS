#!/usr/bin/env bash
#
# Target-local cache for complete ArmOS userland bundles.
#
# Callers provide a bundle name, its build script and the names of direct
# bundle dependencies. The cache validates source inputs, the common target
# ABI, dependency contracts and the produced-file manifest.

hash_build_inputs()
{
    local input_path

    for input_path in "$@"; do
        if [ -f "$input_path" ]; then
            shasum -a 256 "$input_path"
        elif [ -d "$input_path" ]; then
            find "$input_path" -type f -print | LC_ALL=C sort |
                while IFS= read -r input_file; do
                    shasum -a 256 "$input_file"
                done
        else
            printf 'missing %s\n' "$input_path"
        fi
    done
}

build_cached_bundle()
{
    local name="$1"
    local script="$2"
    local work_dir="${WORK_DIR:-$TARGET_BUNDLES/$name}"
    local stamp="$work_dir/.armos-bundle.contract"
    local manifest="$work_dir/.armos-bundle.manifest"
    local source_name="$name"
    local dependency dependency_stamp
    local contract current="" force_bundle=0

    shift 2
    if [ "$name" = "tcc-native" ]; then
        source_name=tcc
    fi
    if [ "${ARMOS_FORCE_RECONFIGURE:-0}" = "1" ]; then
        case "$name" in
            tcc-native|bmake|freetype|fontconfig|ncurses|nano)
                force_bundle=1
                ;;
        esac
    fi
    if [ -z "${ARMOS_BUNDLE_COMMON_CONTRACT:-}" ]; then
        ARMOS_BUNDLE_COMMON_CONTRACT="$(
            {
                printf '%s\n' \
                    "target_arch=$TARGET_ARCH" \
                    "target_platform=$TARGET_PLATFORM" \
                    "arch=$ARCH" \
                    "compiler=$("${ARCH}gcc" --version | head -1)"
                env | LC_ALL=C sort |
                    grep -E '^(ARM_FLAGS|CROSS_COMPILE|HOST_CC|HOSTCC|LIBCXX_INCLUDE|TARGET_)=' ||
                    true
                hash_build_inputs \
                    "$ROOT_DIR/tools/cross_target_env.sh" \
                    "$ROOT_DIR/tools/bundle_cache.sh" \
                    "$ROOT_DIR/userland/include" \
                    "$NEWLIB_LIBC" "$NEWLIB_LIBM" \
                    "$NEWLIB_RUNTIME_DIR" \
                    "$TARGET_BUILD_ROOT/userland/out/usr/lib"
            } | shasum -a 256 | awk '{print $1}'
        )"
    fi
    contract="$(
        {
            printf '%s\n' \
                "bundle=$name" \
                "common=$ARMOS_BUNDLE_COMMON_CONTRACT"
            env | LC_ALL=C sort |
                grep -E '^([A-Z0-9_]+_(PREFIX|VERSION|SHA256|SHA512))=' ||
                true
            hash_build_inputs \
                "$script" \
                "$ROOT_DIR/userland/opt/$source_name" \
                "$ROOT_DIR/third_party/$source_name"
            if [ -n "${ARMOS_BUNDLE_EXTRA_INPUTS:-}" ]; then
                # Paths are repository-owned build inputs and therefore never
                # contain shell whitespace. This keeps optional bundle inputs
                # explicit without teaching the cache about individual ports.
                hash_build_inputs $ARMOS_BUNDLE_EXTRA_INPUTS
            fi
            for dependency in "$@"; do
                dependency_stamp="$TARGET_BUNDLES/$dependency/.armos-bundle.contract"
                hash_build_inputs \
                    "$dependency_stamp" \
                    "$TARGET_BUNDLES/$dependency/bundle"
            done
        } | shasum -a 256 | awk '{print $1}'
    )"

    if [ -f "$stamp" ]; then
        current="$(cat "$stamp")"
    fi
    if [ "${ARMOS_FORCE_USERLAND_REBUILD:-0}" != "1" ] &&
       [ "$force_bundle" != "1" ] &&
       [ "$contract" = "$current" ] &&
       [ -s "$manifest" ]; then
        local cached_path manifest_valid=1
        while IFS= read -r cached_path; do
            if [ ! -e "$work_dir/bundle/$cached_path" ] &&
               [ ! -L "$work_dir/bundle/$cached_path" ]; then
                manifest_valid=0
                break
            fi
        done < "$manifest"
        if [ "$manifest_valid" = "1" ]; then
            echo "=== Bundle cache: reusing $name ==="
            return 0
        fi
    fi

    WORK_DIR="$work_dir" ARCH="$ARCH" NEWLIB_SYSROOT="$NEWLIB_SYSROOT" \
        "$script"
    mkdir -p "$work_dir"
    (
        cd "$work_dir/bundle"
        find . \( -type f -o -type l \) -print | LC_ALL=C sort |
            sed 's|^\./||'
    ) > "$manifest"
    if [ ! -s "$manifest" ]; then
        echo "Error: bundle '$name' produced no files" >&2
        return 1
    fi
    printf '%s\n' "$contract" > "$stamp"
}
