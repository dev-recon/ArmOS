#!/usr/bin/env bash
#
# Shared configure cache for target-local ArmOS bundle builds.
#
# The caller writes the complete configure contract on stdin.  Configure is
# skipped only when that contract still matches and the generated sentinel
# exists.  Stamps live beside the target objects under build/<arch>/<platform>.

ARMOS_CONFIGURE_CACHE_REVISION=2

armos_configure_cache_key()
{
    {
        printf 'configure_cache_revision=%s\n' \
            "$ARMOS_CONFIGURE_CACHE_REVISION"
        cat
    } | shasum -a 256 | awk '{print $1}'
}

armos_configure_reset_build_dir()
{
    local build_dir="$1"

    # Configure output embeds absolute source/build paths in Makefiles,
    # dependency files and generated libtool metadata.  Keeping any of it when
    # the contract changes can therefore mix host and container paths even if
    # all object files are removed.  Only the target-local bundle build tree is
    # discarded; downloaded sources and installed bundle output live beside it.
    if [ -z "$build_dir" ] || [ "$build_dir" = "/" ] ||
       [ "${build_dir#/}" = "$build_dir" ]; then
        echo "error: refusing to reset unsafe configure build directory '$build_dir'" >&2
        return 1
    fi
    mkdir -p "$build_dir" || return 1
    # Keep the directory inode itself: tcc, bmake and ncurses enter BUILD_DIR
    # before consulting the cache.  Removing and recreating that directory
    # would leave those scripts executing from an unlinked working directory.
    find "$build_dir" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} + ||
        return 1
}

armos_configure_needed()
{
    local build_dir="$1"
    local sentinel="$2"
    local contract_file="$build_dir/.armos-configure.contract"
    local pending_file="$build_dir/.armos-configure.contract.pending"
    local new_key old_key="" reason=""

    mkdir -p "$build_dir"
    new_key="$(armos_configure_cache_key)"

    if [ -f "$contract_file" ]; then
        old_key="$(cat "$contract_file")"
    fi

    if [ "${ARMOS_FORCE_RECONFIGURE:-0}" = "1" ]; then
        reason="forced"
    elif [ ! -f "$sentinel" ]; then
        reason="generated files missing"
    elif [ "$new_key" != "$old_key" ]; then
        reason="build contract changed"
    fi

    if [ -z "$reason" ]; then
        rm -f "$pending_file"
        echo "=== Configure cache: reusing $sentinel ==="
        return 1
    fi

    echo "=== Configure cache: $reason; resetting $build_dir ==="
    if ! armos_configure_reset_build_dir "$build_dir"; then
        echo "error: failed to reset configure build directory: $build_dir" >&2
        exit 1
    fi
    printf '%s\n' "$new_key" > "$pending_file"
    return 0
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
