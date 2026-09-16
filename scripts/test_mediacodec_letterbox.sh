#!/usr/bin/env bash
# Compile the production OSD-plane letterbox fill from patch 0106 against a
# recording GL stub. Accept an already patched mpv tree for offline use,
# otherwise fetch the pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

# The slot the fill reads, then the two definitions that do the fill.
python3 "$root/scripts/extract.py" type "$source_dir/video/out/vo_mediacodec.c" \
    "$workdir/mediacodec_letterbox.inc" --name osd_slot
python3 "$root/scripts/extract.py" symbol "$source_dir/video/out/vo_mediacodec.c" \
    "$workdir/mediacodec_letterbox.inc" --append --return int --fn osd_gl_bar
python3 "$root/scripts/extract.py" symbol "$source_dir/video/out/vo_mediacodec.c" \
    "$workdir/mediacodec_letterbox.inc" --append --return void --fn osd_gl_fill_letterbox

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_letterbox.c"
"$workdir/test"
