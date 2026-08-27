#!/bin/sh
# Sourceable helper: fetch an upstream tree at the revision scripts/pins.txt
# records for it. Every build-*-core.sh and build-fbink.sh goes through this
# instead of calling `git clone` itself.
#
# THE POINT IS THAT IT FAILS LOUDLY. The failure mode this replaces is the
# quiet one: `git clone --depth 1 <url>` always succeeds, always builds
# something, and says nothing about the fact that it built a different core
# than the one TESTED.md's numbers came from. Here, a pin that cannot be
# fetched stops the build with the name of the pin in the message, and a
# checkout that lands anywhere other than the pinned commit is an assertion
# failure rather than a build.
#
# `git fetch --depth 1 origin <sha>` into an empty repository is used rather
# than `git clone --depth 1`, which cannot take a commit id at all -- it only
# takes a branch or a tag. The fetch costs the same as the shallow clone it
# replaces (one commit, one tree) and GitHub serves it because it sets
# uploadpack.allowAnySHA1InWant; verified against libretro/gw-libretro before
# this was written, along with the negative case (an unknown sha comes back
# "fatal: remote error: upload-pack: not our ref", exit non-zero).

koboy_pins_file() {
    # Three candidates, in order, and the third is not belt-and-braces. POSIX
    # sh has no BASH_SOURCE, so a sourced file cannot find itself; `$0` is the
    # CALLING script, which is right for every real invocation (the Makefile
    # runs `sh scripts/build-*.sh` from the repository root, and an absolute
    # path by hand resolves the same way) and wrong the moment pins.sh is
    # sourced from an interactive shell or a `sh -c`, where `$0` is "sh" and
    # dirname is ".". The repository-root form covers that without needing the
    # caller to know anything.
    if [ -n "${KOBOY_PINS_FILE:-}" ];        then echo "$KOBOY_PINS_FILE"
    elif [ -f "$(dirname "$0")/pins.txt" ];  then echo "$(dirname "$0")/pins.txt"
    else                                          echo "scripts/pins.txt"
    fi
}

# koboy_pin_lookup <name> <field>   field: 2 = url, 3 = commit
koboy_pin_lookup() {
    _kp_name="$1"; _kp_field="$2"; _kp_file=$(koboy_pins_file)
    [ -f "$_kp_file" ] || {
        echo "FAIL: pin table $_kp_file is missing" >&2; return 1; }
    _kp_out=$(awk -v n="$_kp_name" -v f="$_kp_field" \
        '$1 == n && $1 !~ /^#/ { print $f; found = 1; exit }
         END { if (!found) exit 1 }' "$_kp_file") || {
        echo "FAIL: no pin for '$_kp_name' in $_kp_file" >&2
        echo "      Add a row there; do not clone master as a workaround." >&2
        return 1; }
    echo "$_kp_out"
}

koboy_pin_url() { koboy_pin_lookup "$1" 2; }
koboy_pin_rev() { koboy_pin_lookup "$1" 3; }

# koboy_fetch_pinned <name> <dir> [submodules]
#
# Leaves <dir> checked out at the pinned commit, cloning it if absent. Pass a
# non-empty third argument for a tree with submodules (FBInk); they are
# initialised from the SHAs the pinned commit itself records, so pinning the
# parent pins them too.
koboy_fetch_pinned() {
    _kf_name="$1"; _kf_dir="$2"; _kf_subs="${3:-}"
    _kf_url=$(koboy_pin_url "$_kf_name") || return 1
    _kf_rev=$(koboy_pin_rev "$_kf_name") || return 1

    if [ -d "$_kf_dir/.git" ]; then
        _kf_have=$(git -C "$_kf_dir" rev-parse HEAD 2>/dev/null || echo none)
        if [ "$_kf_have" = "$_kf_rev" ]; then
            # Already exactly right. This is the common case on a developer
            # machine and it must stay free: no fetch, no network, and in
            # particular no checkout, so a working tree carrying the
            # build-fbink.sh Makefile edit is left alone.
            return 0
        fi
        echo "pins: $_kf_name is at $_kf_have, moving to the pinned $_kf_rev" >&2
        git -C "$_kf_dir" cat-file -e "${_kf_rev}^{commit}" 2>/dev/null || \
            git -C "$_kf_dir" fetch --depth 1 origin "$_kf_rev" || {
                echo "FAIL: could not fetch $_kf_name $_kf_rev from $_kf_url" >&2
                return 1; }
        # A dirty tree makes this fail rather than clobber, which is the
        # right way round: local edits to a third_party checkout are somebody
        # debugging, and silently discarding them is worse than stopping.
        git -C "$_kf_dir" checkout -q --detach "$_kf_rev" || {
            echo "FAIL: could not check out $_kf_name $_kf_rev in $_kf_dir" >&2
            echo "      (local modifications? commit, stash or delete the tree)" >&2
            return 1; }
    else
        echo "pins: fetching $_kf_name $_kf_rev" >&2
        mkdir -p "$_kf_dir" || return 1
        git -C "$_kf_dir" init -q || return 1
        git -C "$_kf_dir" remote add origin "$_kf_url" || return 1
        git -C "$_kf_dir" fetch -q --depth 1 origin "$_kf_rev" || {
            echo "FAIL: could not fetch $_kf_name $_kf_rev from $_kf_url" >&2
            echo "      A pin that no longer exists upstream is a repository" >&2
            echo "      that was force-pushed or deleted. Do NOT 'fix' this by" >&2
            echo "      cloning master: find the commit (a fork, a mirror, the" >&2
            echo "      Software Heritage archive) or change the pin knowingly" >&2
            echo "      and re-measure that system's rows in TESTED.md." >&2
            return 1; }
        git -C "$_kf_dir" checkout -q --detach FETCH_HEAD || return 1
    fi

    if [ -n "$_kf_subs" ]; then
        git -C "$_kf_dir" submodule update --init --depth 1 --recursive || {
            echo "FAIL: could not initialise $_kf_name's submodules" >&2
            return 1; }
    fi

    # LIVE ASSERTION, not a comment: every path above claims to have landed on
    # the pin and this is what checks it. A silent build of the wrong tree is
    # the exact defect this whole file exists to remove, so it is worth one
    # rev-parse per core.
    _kf_now=$(git -C "$_kf_dir" rev-parse HEAD 2>/dev/null || echo none)
    [ "$_kf_now" = "$_kf_rev" ] || {
        echo "FAIL: $_kf_dir is at $_kf_now, not the pinned $_kf_rev" >&2
        return 1; }
}
