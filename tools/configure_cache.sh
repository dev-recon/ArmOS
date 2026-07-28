#!/usr/bin/env bash
#
# Shared configure cache for target-local ArmOS bundle builds.
#
# The caller writes the complete configure contract on stdin.  Configure is
# skipped only when that contract still matches and the generated sentinel
# exists.  Stamps live beside the target objects under build/<arch>/<platform>.

armos_configure_cache_key()
{
    shasum -a 256 | awk '{print $1}'
}

armos_configure_invalidate_objects()
{
    local build_dir="$1"

    # A changed configure contract can alter ABI or compiler flags.  Preserve
    # generated configuration files, but never reuse objects from that contract.
    find "$build_dir" -type f \( \
        -name '*.o' -o -name '*.lo' -o -name '*.a' -o -name '*.la' \
        \) -delete
}

armos_configure_needed()
{
    local build_dir="$1"
    local sentinel="$2"
    local contract_file="$build_dir/.armos-configure.contract"
    local pending_file="$build_dir/.armos-configure.contract.pending"
    local new_key old_key=""

    mkdir -p "$build_dir"
    new_key="$(armos_configure_cache_key)"
    printf '%s\n' "$new_key" > "$pending_file"

    if [ -f "$contract_file" ]; then
        old_key="$(cat "$contract_file")"
    fi

    if [ "${ARMOS_FORCE_RECONFIGURE:-0}" = "1" ]; then
        echo "=== Configure cache: forced ==="
        armos_configure_invalidate_objects "$build_dir"
        return 0
    fi
    if [ ! -f "$sentinel" ]; then
        echo "=== Configure cache: generated files missing ==="
        return 0
    fi
    if [ "$new_key" != "$old_key" ]; then
        echo "=== Configure cache: build contract changed ==="
        armos_configure_invalidate_objects "$build_dir"
        return 0
    fi

    rm -f "$pending_file"
    echo "=== Configure cache: reusing $sentinel ==="
    return 1
}

armos_configure_commit()
{
    local build_dir="$1"
    local contract_file="$build_dir/.armos-configure.contract"
    local pending_file="$build_dir/.armos-configure.contract.pending"

    if [ ! -f "$pending_file" ]; then
        echo "error: configure cache has no pending contract" >&2
        return 1
    fi
    mv "$pending_file" "$contract_file"
}
