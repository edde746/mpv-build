#!/usr/bin/env bash
# Build the pinned libass with its series for the host and run the series'
# behavioral harnesses against it: a static frame rendered twice is reported
# unchanged the second time (the layout cache must not hand the next frame a
# copy with a fresh bitmap pointer), and a cached shaping result is only reused
# for a run whose every HarfBuzz input matches. Pass an already-patched libass
# tree for offline use; otherwise
# fetch and patch the pinned source in a temporary directory. Needs the host's
# freetype, fribidi, harfbuzz and libunibreak via pkg-config, autotools, and a
# system font.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

for pkg in freetype2 fribidi harfbuzz libunibreak; do
    pkg-config --exists "$pkg" || { echo "FAIL: host $pkg not found via pkg-config" >&2; exit 1; }
done

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved libass/linux series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned libass linux "$source_dir")
fi

# This harness builds libass for the HOST, so it applies series.common and
# nothing else. libass also declares series.linux and series.windows; if either
# ever gains an entry, the host build here would silently stop matching what the
# linux or windows driver applies, so refuse rather than diverge.
#
# The guard compares the resolved series against series.common, so it can only
# say anything if the resolution is real. patches.py resolves patches/ against
# the cwd, so a call from anywhere but the repo root reads no series at all and
# returns an empty list for every platform -- which compares equal to an empty
# platform series and passes without checking anything. Resolve from $root, and
# refuse an empty or truncated resolution instead of comparing empty to empty.
common="$(grep -v '^#' "$root/patches/libass/series.common" | grep . || true)"
for platform in linux windows; do
    (cd "$root" && python3 scripts/patches.py resolve libass "$platform") >"$workdir/series.$platform"
    resolved="$(cat "$workdir/series.$platform")"
    if [[ -z "$resolved" ]] \
        || [[ -n "$(comm -23 <(printf '%s\n' "$common" | sort) <(printf '%s\n' "$resolved" | sort))" ]]; then
        echo "FAIL: resolving libass/$platform did not return series.common's entries;" >&2
        echo "      patches.py resolves patches/ against the cwd, so this must run" >&2
        echo "      from $root, and the resolution is series.common plus series.$platform." >&2
        exit 1
    fi
    if [[ -n "$(comm -13 <(printf '%s\n' "$common" | sort) <(printf '%s\n' "$resolved" | sort))" ]]; then
        echo "FAIL: patches/libass/series.$platform is no longer empty; this harness applies" >&2
        echo "      series.common only, so it must learn about that patch first." >&2
        exit 1
    fi
done

build="$workdir/build"
mkdir -p "$build"
(
    cd "$source_dir"
    [[ -f configure ]] || ./autogen.sh >/dev/null 2>&1
    cd "$build"
    "$source_dir/configure" --disable-shared --enable-static --with-pic \
        --disable-require-system-font-provider --disable-fontconfig >"$build/configure.log" 2>&1 \
        || { cat "$build/configure.log" >&2; exit 1; }
    make -j"$(getconf _NPROCESSORS_ONLN)" >"$build/make.log" 2>&1 \
        || { tail -40 "$build/make.log" >&2; exit 1; }
)

link=()
case "$(uname -s)" in
    Darwin) link=(-liconv -framework CoreText -framework CoreFoundation) ;;
esac
for harness in test_libass_change_detection test_libass_shape_cache; do
    cc -O1 -std=c11 -Wall -Wextra -Werror -I"$source_dir" \
        -o "$workdir/$harness" "$root/scripts/$harness.c" \
        "$build/libass/.libs/libass.a" \
        $(pkg-config --libs freetype2 fribidi harfbuzz libunibreak) \
        "${link[@]}" -lpthread -lm
    "$workdir/$harness"
done
