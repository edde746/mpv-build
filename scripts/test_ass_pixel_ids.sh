#!/usr/bin/env bash
# Compile the ids mp_ass_packer gives libass bitmaps for their pixels (the
# marked "ass pixel ids" region of sub/ass_mp.c, patch 0234) VERBATIM from the
# final Apple series. Pass an already-patched mpv tree for offline use;
# otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/sub/ass_mp.c" \
    "$workdir/ass_pixel_ids.inc" --start "ass pixel ids" \
    --require "assign_pixels_ids" --require "next_pixels_id"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_ass_pixel_ids.c"
"$workdir/test"
