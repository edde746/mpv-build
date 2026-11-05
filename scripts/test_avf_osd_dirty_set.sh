#!/usr/bin/env bash
# Compile vo_avfoundation's per-buffer OSD dirty set (the marked "avf osd
# dirty set" region, patch 0043) VERBATIM from the final Apple series. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/video/out/vo_avfoundation.m" \
    "$workdir/avf_osd_dirty_set.inc" --start "avf osd dirty set" \
    --require "avf_dirty_add" --require "avf_dirty_pixels"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_osd_dirty_set.c"
"$workdir/test"
