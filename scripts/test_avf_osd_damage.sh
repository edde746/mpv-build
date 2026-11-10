#!/usr/bin/env bash
# Compile vo_avfoundation's per-buffer OSD damage (the marked "avf osd damage"
# region, patch 0048) VERBATIM from the final Apple series. Pass an
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
    "$workdir/avf_osd_damage.inc" --start "avf osd damage" \
    --require "avf_sig_add" --require "avf_damage_apply"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_osd_damage.c"
"$workdir/test"
