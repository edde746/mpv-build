#!/usr/bin/env bash
# Compile what demux_mkv reads behind the clusters when it opens a file and on
# the first seek (read_trailing_headers(), read_deferred_cues() and the tag
# handling, patch 0240) against a recorded stream: Tags stored after the
# clusters wait for the Cues on a network stream and nowhere else, and tags
# read after opening reach only the global metadata. Pass an already-patched
# mpv tree for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0240 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
mkv="$source_dir/demux/demux_mkv.c"
python3 "$E" range "$mkv" "$workdir/demux_mkv_types.inc" \
    --first 'typedef struct mkv_demuxer {' --last '} mkv_demuxer_t;' --last-inclusive
python3 "$E" type "$mkv" "$workdir/demux_mkv_types.inc" --append --name demux_mkv_opts
inc="$workdir/demux_mkv_defer_tags.inc"
python3 "$E" symbol "$mkv" "$inc" --fn process_tags --return void
python3 "$E" symbol "$mkv" "$inc" --append --fn read_deferred_element --return int
python3 "$E" symbol "$mkv" "$inc" --append --fn trailing_headers_can_wait --return bool
python3 "$E" symbol "$mkv" "$inc" --append --fn read_deferred_cues --return void
python3 "$E" symbol "$mkv" "$inc" --append --fn read_trailing_headers --return int
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -I"$workdir" "$root/scripts/test_demux_mkv_defer_tags.c" -o "$workdir/test"
"$workdir/test"
